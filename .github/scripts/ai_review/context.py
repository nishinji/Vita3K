"""Builds the review prompt: the diff plus the surrounding code a reviewer would open."""

from __future__ import annotations

import os
import re
from collections import Counter

from .diff import FileDiff
from .index import RepoIndex

CALL_RE = re.compile(r"\b([A-Za-z_][A-Za-z0-9_]{3,})\s*\(")
TYPE_RE = re.compile(r"\b([A-Z][A-Za-z0-9_]{2,})\b")
DEFINES_RE = (
    re.compile(r"^[ \t]*(?:EXPORT|EXPORT_UNIMPLEMENTED|BRIDGE_IMPL)[ \t]*\([^,]+,[ \t]*([A-Za-z_]\w+)"),
    re.compile(r"^[A-Za-z_][\w:<>,&*\s]*?\b([A-Za-z_]\w+)\s*\([^;]*\)[^;]*\{?[ \t]*$"),
    re.compile(r"^[ \t]*(?:class|struct|union|enum(?:[ \t]+class)?)[ \t]+(?:[A-Z_0-9]+[ \t]+)?([A-Za-z_]\w+)"),
)

# Names that are everywhere and never worth a cross-reference lookup.
STOPWORDS = {
    "alignas", "alignof", "auto", "bool", "break", "case", "catch", "char", "class", "const",
    "constexpr", "continue", "decltype", "default", "delete", "double", "else", "enum", "explicit",
    "export", "extern", "false", "float", "for", "friend", "goto", "if", "inline", "int", "long",
    "mutable", "namespace", "new", "noexcept", "nullptr", "operator", "private", "protected",
    "public", "register", "reinterpret_cast", "return", "short", "signed", "sizeof", "static",
    "static_assert", "static_cast", "struct", "switch", "template", "this", "throw", "true", "try",
    "typedef", "typename", "union", "unsigned", "using", "virtual", "void", "volatile", "while",
    "assert", "memcpy", "memset", "printf", "sprintf", "snprintf", "strlen", "free", "malloc",
    "std", "size_t", "uint8_t", "uint16_t", "uint32_t", "uint64_t", "int8_t", "int16_t", "int32_t",
    "int64_t", "string", "vector", "shared_ptr", "unique_ptr", "make_shared", "make_unique",
    "LOG_ERROR", "LOG_WARN", "LOG_INFO", "LOG_DEBUG", "LOG_TRACE", "LOG_CRITICAL", "TRACY_FUNC",
    "STUBBED", "UNIMPLEMENTED", "SCE_KERNEL_ERROR_OK", "RET_ERROR", "RET_UNIMPLEMENTED",
    "TODO", "NOTE", "FIXME", "Copyright", "This", "The", "You", "GNU", "General", "Public", "License",
    "begin", "end", "size", "data", "count", "clear", "reset", "init", "check", "value", "name",
    "type", "find", "insert", "erase", "empty", "resize", "swap", "push_back", "emplace_back",
    "to_string", "transform", "move", "forward", "get", "set", "read", "write", "open", "close",
    "update", "main", "first", "second", "index", "result", "offset", "length", "width", "height",
}
# Above this many mentioning files a symbol is generic, and its "definition" would be arbitrary.
MAX_SYMBOL_FREQUENCY = 40
# Generated tables produce reference hits that carry no review value.
NOISE_RE = re.compile(r"(^|/)(nids|external|build)/|\.inc$")
CODE_EXTS = {".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx", ".inl", ".m", ".mm", ".java", ".kt"}

MAX_CHANGED_FILE_CHARS = 60_000
COUNTERPART_CHARS = 24_000
DEFINITION_RADIUS = 30
CALLER_RADIUS = 14


class Budget:
    """Fills a section list in priority order without blowing the total prompt size."""

    def __init__(self, total: int):
        self.remaining = total

    def take(self, text: str, cap: int) -> str | None:
        room = min(cap, self.remaining)
        if room <= 400:
            return None
        if len(text) > room:
            text = text[:room] + "\n... [truncated]"
        self.remaining -= len(text)
        return text


def _fenced(path: str, body: str) -> str:
    lang = {
        ".c": "c", ".h": "cpp", ".cpp": "cpp", ".hpp": "cpp", ".cc": "cpp", ".hh": "cpp",
        ".inl": "cpp", ".py": "python", ".cmake": "cmake", ".yml": "yaml", ".yaml": "yaml",
    }.get(os.path.splitext(path)[1].lower(), "")
    return f"```{lang}\n{body}\n```"


def numbered(idx: RepoIndex, path: str) -> str:
    return idx.snippet(path, 1, idx.line_count(path))


def candidate_symbols(files: list[FileDiff]) -> tuple[list[str], list[str]]:
    """Identifiers the diff uses (resolve elsewhere) and identifiers the diff defines (find callers)."""
    used: Counter[str] = Counter()
    defined: Counter[str] = Counter()

    for file_diff in files:
        if os.path.splitext(file_diff.path)[1].lower() not in CODE_EXTS:
            continue
        for line in file_diff.changed_lines():
            for pattern in DEFINES_RE:
                match = pattern.match(line)
                if match:
                    defined[match.group(1)] += 1
                    break
            used.update(CALL_RE.findall(line))
            used.update(TYPE_RE.findall(line))
        # The enclosing declaration is what the change modifies, so look for its users elsewhere.
        for hunk in file_diff.hunks:
            defined.update(TYPE_RE.findall(hunk.section))
            defined.update(CALL_RE.findall(hunk.section))

    # A symbol can be in both lists: for a changed signature its definition and its callers both matter.
    defined_names = [n for n, _ in defined.most_common() if n not in STOPWORDS][:12]
    used_names = [
        name for name, _ in used.most_common()
        if name not in STOPWORDS and not name.isupper()
    ][:24]
    return used_names, defined_names


def _related_blocks(files: list[FileDiff], idx: RepoIndex) -> list[tuple[str, str]]:
    """(heading, body) pairs of code that a reviewer of this diff would want open."""
    changed = {f.path for f in files}
    seen: set[str] = set()
    blocks: list[tuple[str, str]] = []

    for file_diff in files:
        for counterpart in idx.counterparts(file_diff.path)[:2]:
            if counterpart in changed or counterpart in seen or not idx.has(counterpart):
                continue
            seen.add(counterpart)
            body = numbered(idx, counterpart)
            if len(body) > COUNTERPART_CHARS:
                body = body[:COUNTERPART_CHARS] + "\n... [truncated]"
            blocks.append((f"{counterpart} — counterpart of {file_diff.path}", body))

    used, defined = candidate_symbols(files)
    # A definition site is unique and worth having even for a common type, but the "callers" of a
    # common name are arbitrary, so only the reference lookup is frequency-gated.
    defined = [s for s in defined if idx.frequency(s) < MAX_SYMBOL_FREQUENCY]

    for symbol in used:
        for hit in idx.definitions(symbol, changed, limit=1):
            key = f"{hit.path}:{hit.line}"
            if key in seen or NOISE_RE.search(hit.path):
                continue
            seen.add(key)
            snippet = idx.snippet(hit.path, hit.line - 4, hit.line + DEFINITION_RADIUS)
            if snippet:
                blocks.append((f"{hit.path}:{hit.line} — definition of `{symbol}` used by the diff", snippet))

    for symbol in defined:
        for hit in idx.references(symbol, changed, limit=2):
            key = f"{hit.path}:{hit.line}"
            if key in seen or NOISE_RE.search(hit.path):
                continue
            seen.add(key)
            snippet = idx.snippet(hit.path, hit.line - CALLER_RADIUS, hit.line + CALLER_RADIUS)
            if snippet:
                blocks.append((f"{hit.path}:{hit.line} — existing caller of `{symbol}`", snippet))

    return blocks


def _dependents(files: list[FileDiff], idx: RepoIndex) -> str:
    lines: list[str] = []
    for file_diff in files:
        if os.path.splitext(file_diff.path)[1].lower() not in (".h", ".hpp", ".hh", ".inl"):
            continue
        hits = [h.path for h in idx.includers(file_diff.path, limit=10)]
        if hits:
            lines.append(f"- `{file_diff.path}` is included by: " + ", ".join(f"`{p}`" for p in hits))
    return "\n".join(lines)


def repo_map(idx: RepoIndex, limit: int = 60) -> str:
    modules: Counter[str] = Counter()
    for path in idx.text:
        parts = path.split("/")
        modules["/".join(parts[:2]) if len(parts) > 2 else parts[0]] += 1
    rows = [f"- `{name}/` ({count} files)" for name, count in sorted(modules.items()) if count > 1]
    return "\n".join(rows[:limit])


def build(pull: dict, files: list[FileDiff], idx: RepoIndex, total_budget: int) -> str:
    budget = Budget(total_budget)
    out: list[str] = []

    head = pull.get("head", {}) or {}
    base = pull.get("base", {}) or {}
    out.append(
        "# Pull request\n\n"
        f"- Repository: {(base.get('repo') or {}).get('full_name', '?')}\n"
        f"- Title: {pull.get('title', '')}\n"
        f"- Author: {(pull.get('user') or {}).get('login', '?')}\n"
        f"- Branch: `{head.get('ref', '?')}` -> `{base.get('ref', '?')}`\n\n"
        "## Description\n\n"
        + (pull.get("body") or "_(none)_")[:4000]
    )

    out.append("# Repository layout\n\n" + repo_map(idx))

    listing = "\n".join(
        f"- `{f.path}` ({f.status}, {sum(1 for h in f.hunks for l in h.lines if l.startswith('+'))} added / "
        f"{sum(1 for h in f.hunks for l in h.lines if l.startswith('-'))} removed)"
        for f in files
    )
    out.append("# Changed files\n\n" + listing)

    patches = "\n\n".join(f"### {f.path}\n```diff\n{f.patch()}\n```" for f in files if f.hunks)
    taken = budget.take(patches, int(total_budget * 0.45))
    out.append("# Diff\n\nLine numbers in the hunk headers are authoritative for review anchors.\n\n" + (taken or "_(omitted: too large)_"))

    bodies: list[str] = []
    body_cap = int(total_budget * 0.35)
    body_used = 0
    for file_diff in files:
        if file_diff.status == "removed":
            continue
        content = numbered(idx, file_diff.path)
        if not content:
            continue
        if len(content) > MAX_CHANGED_FILE_CHARS:
            windows = [idx.snippet(file_diff.path, s - 40, e + 40) for s, e in file_diff.touched_ranges()]
            content = "\n...\n".join(w for w in windows if w)
        block = f"### {file_diff.path}\n{_fenced(file_diff.path, content)}"
        if body_used + len(block) > body_cap:
            continue
        body_used += len(block)
        bodies.append(block)
    if bodies:
        budget.remaining -= body_used
        out.append(
            "# Changed files after the change (full text, `line | code`)\n\n"
            "Use these line numbers when reporting a finding.\n\n" + "\n\n".join(bodies)
        )

    related: list[str] = []
    for heading, body in _related_blocks(files, idx):
        block = f"### {heading}\n```cpp\n{body}\n```"
        taken = budget.take(block, len(block))
        if taken is None:
            break
        related.append(taken)
    if related:
        out.append(
            "# Related code (unchanged, for context)\n\n"
            "Counterpart headers, definitions the diff calls into, and existing callers of what it defines.\n\n"
            + "\n\n".join(related)
        )

    dependents = _dependents(files, idx)
    if dependents:
        out.append("# Reverse dependencies of changed headers\n\n" + dependents)

    return "\n\n".join(out)
