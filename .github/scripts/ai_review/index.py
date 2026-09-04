"""In-memory index of the reviewed checkout, used to pull related code into the prompt."""

from __future__ import annotations

import os
import re
from dataclasses import dataclass

SOURCE_EXTS = {
    ".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx", ".inl", ".inc",
    ".m", ".mm", ".java", ".kt", ".py", ".sh", ".cmake", ".yml", ".yaml",
    ".glsl", ".vert", ".frag", ".comp", ".rs",
}
SOURCE_NAMES = {"CMakeLists.txt", "CMakePresets.json"}
EXCLUDE_DIRS = {
    ".git", "external", "build", "dist", "data", "docs", "vcpkg",
    "_building", "_readme", "i18n", "node_modules",
}
# Dot directories are skipped by default, but CI definitions are worth reviewing in context.
DOT_DIRS_KEPT = {".github", ".ci"}
HEADER_EXTS = (".h", ".hpp", ".hh", ".hxx", ".inl")
IMPL_EXTS = (".cpp", ".c", ".cc", ".cxx", ".mm", ".m")
MAX_INDEXED_BYTES = 512 * 1024

_INCLUDE = re.compile(r'^[ \t]*#[ \t]*include[ \t]+[<"]([^>"]+)[>"]', re.MULTILINE)

# Ordered by confidence. Forward declarations are deliberately not matched: they carry no context.
DEFINITION_PATTERNS = (
    r"(?m)^[ \t]*(?:EXPORT|EXPORT_UNIMPLEMENTED|BRIDGE_IMPL|BRIDGE_DECL)[ \t]*\([^,()\n]*,[ \t]*{sym}[ \t]*[,)]",
    r"(?m)^[ \t]*(?:template[ \t]*<[^>\n]*>[ \t]*)?"
    r"(?:class|struct|union|enum(?:[ \t]+class)?)[ \t]+(?:[A-Z_0-9]+[ \t]+)?{sym}\b[ \t]*(?::[^;\n]*)?[ \t]*\{{?[ \t]*$",
    r"(?m)^[ \t]*using[ \t]+{sym}[ \t]*=",
    r"(?m)^[ \t]*typedef\b[^\n;]*\b{sym}[ \t]*(?:\[[^\]\n]*\])?[ \t]*;",
    r"(?m)^[ \t]*#[ \t]*define[ \t]+{sym}\b",
    r"(?m)^[ \t]*(?:inline[ \t]+|static[ \t]+|const[ \t]+|constexpr[ \t]+)*constexpr\b[^\n=;]*\b{sym}[ \t]*[=\[{{]",
    r"(?m)^[A-Za-z_][\w:<>,&*\s]*?\b{sym}\s*\(",
)
REFERENCE_PATTERN = r"\b{sym}\s*[({{\[.,;)]"


@dataclass(frozen=True)
class Hit:
    path: str
    line: int


def _is_source(name: str) -> bool:
    return name in SOURCE_NAMES or os.path.splitext(name)[1].lower() in SOURCE_EXTS


class RepoIndex:
    """Whole-checkout text index. The tree outside external/ is a few MB, so eager loading is fine."""

    def __init__(self, root: str):
        self.root = os.path.abspath(root)
        self.text: dict[str, str] = {}
        self.by_stem: dict[str, list[str]] = {}
        self.by_name: dict[str, list[str]] = {}
        self._load()

    def _load(self) -> None:
        for dirpath, dirnames, filenames in os.walk(self.root):
            dirnames[:] = [
                d for d in dirnames
                if d not in EXCLUDE_DIRS and (not d.startswith(".") or d in DOT_DIRS_KEPT)
            ]
            for name in sorted(filenames):
                if not _is_source(name):
                    continue
                full = os.path.join(dirpath, name)
                try:
                    if os.path.getsize(full) > MAX_INDEXED_BYTES:
                        continue
                    with open(full, "r", encoding="utf-8", errors="replace") as handle:
                        body = handle.read()
                except OSError:
                    continue
                rel = os.path.relpath(full, self.root).replace(os.sep, "/")
                self.text[rel] = body
                self.by_name.setdefault(name, []).append(rel)
                self.by_stem.setdefault(os.path.splitext(name)[0], []).append(rel)

        # Headers first: a declaration is a better answer than the first matching call site.
        self.scan_order = sorted(
            self.text, key=lambda p: (os.path.splitext(p)[1].lower() not in HEADER_EXTS, p)
        )

    # -- file access -------------------------------------------------------

    def has(self, path: str) -> bool:
        return path in self.text

    def read(self, path: str) -> str | None:
        if path in self.text:
            return self.text[path]
        full = os.path.join(self.root, path.replace("/", os.sep))
        try:
            if os.path.getsize(full) > 4 * MAX_INDEXED_BYTES:
                return None
            with open(full, "r", encoding="utf-8", errors="replace") as handle:
                return handle.read()
        except OSError:
            return None

    def line_count(self, path: str) -> int:
        body = self.read(path)
        return len(body.splitlines()) if body else 0

    def snippet(self, path: str, start: int, end: int) -> str:
        body = self.read(path)
        if body is None:
            return ""
        lines = body.splitlines()
        start = max(1, start)
        end = min(len(lines), end)
        if start > end:
            return ""
        width = len(str(end))
        return "\n".join(f"{n:>{width}} | {lines[n - 1]}" for n in range(start, end + 1))

    # -- relationships -----------------------------------------------------

    def counterparts(self, path: str) -> list[str]:
        """Header for a translation unit and vice versa, matched by stem across the tree."""
        stem, ext = os.path.splitext(os.path.basename(path))
        wanted = IMPL_EXTS if ext.lower() in HEADER_EXTS else HEADER_EXTS
        return [
            candidate
            for candidate in self.by_stem.get(stem, [])
            if candidate != path and os.path.splitext(candidate)[1].lower() in wanted
        ]

    def local_includes(self, path: str, limit: int = 12) -> list[str]:
        """Project headers the file includes; system headers simply fail to resolve."""
        body = self.read(path)
        if body is None:
            return []
        found: list[str] = []
        for raw in _INCLUDE.findall(body):
            resolved = self._resolve_include(path, raw.replace("\\", "/"))
            if resolved and resolved not in found:
                found.append(resolved)
            if len(found) >= limit:
                break
        return found

    def _resolve_include(self, origin: str, target: str) -> str | None:
        sibling = os.path.normpath(os.path.join(os.path.dirname(origin), target)).replace(os.sep, "/")
        if sibling in self.text:
            return sibling
        for candidate in self.by_name.get(os.path.basename(target), []):
            if candidate == target or candidate.endswith("/" + target):
                return candidate
        return None

    @staticmethod
    def include_path(path: str) -> str:
        """How the tree spells this header in an #include: the part after an include root."""
        marker = "/include/"
        cut = path.rfind(marker)
        return path[cut + len(marker):] if cut >= 0 else os.path.basename(path)

    def includers(self, path: str, limit: int = 8) -> list[Hit]:
        """Files that include this header, i.e. the code a header change can break."""
        spelling = self.include_path(path)
        name = os.path.basename(path)
        hits: list[Hit] = []
        for other, body in self.text.items():
            if other == path or name not in body:
                continue
            for match in _INCLUDE.finditer(body):
                target = match.group(1).replace("\\", "/")
                if target == spelling or self._resolve_include(other, target) == path:
                    hits.append(Hit(other, body.count("\n", 0, match.start()) + 1))
                    break
            if len(hits) >= limit:
                break
        return hits

    # -- symbol lookup -----------------------------------------------------

    def frequency(self, symbol: str, cap: int = 64) -> int:
        """How many files mention the symbol; a high count means it is too generic to cross-reference."""
        count = 0
        for body in self.text.values():
            if symbol in body:
                count += 1
                if count >= cap:
                    break
        return count

    def definitions(self, symbol: str, exclude: set[str], limit: int = 3) -> list[Hit]:
        for pattern in DEFINITION_PATTERNS:
            hits = self._scan(symbol, pattern, exclude, limit)
            if hits:
                return hits
        return []

    def references(self, symbol: str, exclude: set[str], limit: int = 3) -> list[Hit]:
        return self._scan(symbol, REFERENCE_PATTERN, exclude, limit)

    def _scan(self, symbol: str, pattern: str, exclude: set[str], limit: int) -> list[Hit]:
        regex = re.compile(pattern.format(sym=re.escape(symbol)))
        hits: list[Hit] = []
        for path in self.scan_order:
            body = self.text[path]
            if path in exclude or symbol not in body:
                continue
            match = regex.search(body)
            if match:
                hits.append(Hit(path, body.count("\n", 0, match.start()) + 1))
                if len(hits) >= limit:
                    break
        return hits
