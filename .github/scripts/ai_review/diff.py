"""Unified diff parsing, stdlib only."""

from __future__ import annotations

import re
from dataclasses import dataclass, field

_HUNK = re.compile(r"^@@ -(\d+)(?:,(\d+))? \+(\d+)(?:,(\d+))? @@")
_GIT_HEADER = re.compile(r"^diff --git a/(.+) b/(.+)$")
_IDENT = re.compile(r"[A-Za-z_][A-Za-z0-9_]*")


@dataclass
class Hunk:
    old_start: int
    old_count: int
    new_start: int
    new_count: int
    header: str
    lines: list[str] = field(default_factory=list)

    @property
    def new_end(self) -> int:
        return self.new_start + max(self.new_count, 1) - 1

    @property
    def section(self) -> str:
        """Text git puts after the hunk marker: the declaration that encloses the change."""
        _, _, tail = self.header.partition("@@")
        return tail.partition("@@")[2].strip()


@dataclass
class FileDiff:
    path: str
    old_path: str
    status: str = "modified"
    binary: bool = False
    hunks: list[Hunk] = field(default_factory=list)

    def patch(self) -> str:
        out: list[str] = []
        for hunk in self.hunks:
            out.append(hunk.header)
            out.extend(hunk.lines)
        return "\n".join(out)

    def right_lines(self) -> set[int]:
        """New-file line numbers present in the diff: the only valid inline comment anchors."""
        seen: set[int] = set()
        for hunk in self.hunks:
            lineno = hunk.new_start
            for line in hunk.lines:
                if line.startswith("\\"):
                    continue
                if line.startswith("-"):
                    continue
                seen.add(lineno)
                lineno += 1
        return seen

    def touched_ranges(self, pad: int = 0) -> list[tuple[int, int]]:
        ranges = [(max(1, h.new_start - pad), h.new_end + pad) for h in self.hunks if h.new_count]
        return _merge_ranges(ranges)

    def changed_lines(self) -> list[str]:
        """Payload of added and removed lines, prefix stripped."""
        out: list[str] = []
        for hunk in self.hunks:
            for line in hunk.lines:
                if line[:1] in ("+", "-"):
                    out.append(line[1:])
        return out

    def added_text(self) -> str:
        out: list[str] = []
        for hunk in self.hunks:
            out.extend(line[1:] for line in hunk.lines if line.startswith("+"))
        return "\n".join(out)

    def identifiers(self) -> set[str]:
        return set(_IDENT.findall("\n".join(self.changed_lines())))


def _merge_ranges(ranges: list[tuple[int, int]]) -> list[tuple[int, int]]:
    merged: list[tuple[int, int]] = []
    for start, end in sorted(ranges):
        if merged and start <= merged[-1][1] + 1:
            merged[-1] = (merged[-1][0], max(merged[-1][1], end))
        else:
            merged.append((start, end))
    return merged


def parse(text: str) -> list[FileDiff]:
    files: list[FileDiff] = []
    current: FileDiff | None = None
    hunk: Hunk | None = None

    for line in text.splitlines():
        header = _GIT_HEADER.match(line)
        if header:
            current = FileDiff(path=header.group(2), old_path=header.group(1))
            files.append(current)
            hunk = None
            continue
        if current is None:
            continue

        if line.startswith("new file mode"):
            current.status = "added"
        elif line.startswith("deleted file mode"):
            current.status = "removed"
        elif line.startswith("rename to "):
            current.status = "renamed"
            current.path = line[len("rename to "):]
        elif line.startswith("rename from "):
            current.old_path = line[len("rename from "):]
        elif line.startswith("GIT binary patch") or line.startswith("Binary files "):
            current.binary = True
        elif line.startswith("--- "):
            hunk = None
            if line != "--- /dev/null":
                current.old_path = line[6:]
        elif line.startswith("+++ "):
            hunk = None
            if line != "+++ /dev/null":
                current.path = line[6:]
        else:
            match = _HUNK.match(line)
            if match:
                hunk = Hunk(
                    old_start=int(match.group(1)),
                    old_count=int(match.group(2) or 1),
                    new_start=int(match.group(3)),
                    new_count=int(match.group(4) or 1),
                    header=line,
                )
                current.hunks.append(hunk)
            elif hunk is not None and line[:1] in (" ", "+", "-", "\\", ""):
                hunk.lines.append(line)

    return [f for f in files if f.hunks or f.binary]
