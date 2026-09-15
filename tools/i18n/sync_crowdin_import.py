from __future__ import annotations

import argparse
from dataclasses import dataclass
import json
import os
from pathlib import Path
import subprocess

CATALOG_PREFIX = "overlay_"
SOURCE_TAG = "en"


@dataclass
class Imported:
    tag: str
    path: Path
    percent: int = 0
    is_new: bool = False
    dropped: bool = False
    error: str = ""


def load_catalog(path: Path) -> dict[str, str]:
    raw = json.loads(path.read_text(encoding="utf-8"))
    messages: dict[str, str] = {}

    for key, value in raw.items():
        if isinstance(value, str):
            messages[key] = value
        elif isinstance(value, dict) and isinstance(value.get("message"), str):
            messages[key] = value["message"]
        else:
            raise RuntimeError(f"Unsupported catalog entry: {key}")

    return messages


def count_translated(path: Path, source_keys: set[str]) -> int:
    messages = load_catalog(path)
    return sum(1 for key in source_keys if messages.get(key, "").strip())


def tracked_names(repo_root: Path, directory: Path) -> set[str]:
    listing = subprocess.run(
        ["git", "-C", str(repo_root), "ls-files", "--", str(directory)],
        capture_output=True,
        text=True,
        check=True,
    )

    return {Path(line).name for line in listing.stdout.splitlines() if line}


def collect(lang_dir: Path, repo_root: Path, source_keys: set[str]) -> list[Imported]:
    tracked = tracked_names(repo_root, lang_dir)
    results: list[Imported] = []

    for path in sorted(lang_dir.glob(f"{CATALOG_PREFIX}*.json")):
        tag = path.stem[len(CATALOG_PREFIX):]
        if tag == SOURCE_TAG:
            continue

        entry = Imported(tag=tag, path=path, is_new=path.name not in tracked)
        try:
            entry.percent = min(100, count_translated(path, source_keys) * 100 // len(source_keys))
        except Exception as error:
            entry.error = f"{path.name}: {error}"

        results.append(entry)

    return results


def drop_unfinished(entries: list[Imported], min_completion: int) -> None:
    for entry in entries:
        if entry.error or not entry.is_new or entry.percent >= min_completion:
            continue

        entry.path.unlink()
        entry.dropped = True


def render_report(entries: list[Imported], min_completion: int) -> str:
    lines = ["| Language | Translated |", "| --- | --- |"]
    for entry in entries:
        if entry.dropped:
            continue

        label = f"`{entry.tag}`" + (" (new)" if entry.is_new else "")
        lines.append(f"| {label} | {'unreadable' if entry.error else f'{entry.percent}%'} |")

    report = ["\n".join(lines), ""]

    dropped = sorted(entry.path.name for entry in entries if entry.dropped)
    if dropped:
        report.append(
            f"Left out because they are new and under {min_completion}% translated: "
            + ", ".join(f"`{name}`" for name in dropped)
            + ".\n"
        )

    added = sorted(entry.tag for entry in entries if entry.is_new and not entry.dropped and not entry.error)
    if added:
        report.append(
            "New languages: "
            + ", ".join(f"`{tag}`" for tag in added)
            + ". The overlay follows the emulated system language, so a language missing from"
            " `locale_tag_for_sys_lang` (`vita3k/lang/src/lang.cpp`) can never be selected.\n"
        )

    errors = [entry.error for entry in entries if entry.error]
    if errors:
        report.append("> [!CAUTION]")
        report.append("> Crowdin returned catalogs that could not be read:")
        report.extend(f"> - {error}" for error in errors)
        report.append("")

    return "\n".join(report)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", default=str(Path(__file__).resolve().parents[2]))
    parser.add_argument("--lang-dir", default="i18n/lang")
    parser.add_argument("--min-completion", type=int, default=80)
    parser.add_argument("--report")
    args = parser.parse_args()

    repo_root = Path(args.repo_root).resolve()
    lang_dir = repo_root / args.lang_dir

    source_keys = set(load_catalog(lang_dir / f"{CATALOG_PREFIX}{SOURCE_TAG}.json"))
    if not source_keys:
        raise RuntimeError("The English catalog carries no strings to measure the import against")

    entries = collect(lang_dir, repo_root, source_keys)
    drop_unfinished(entries, args.min_completion)

    report = render_report(entries, args.min_completion)
    print(report)

    if args.report:
        with open(args.report, "a", encoding="utf-8") as handle:
            handle.write(report)

    blocking = any(entry.error for entry in entries)
    github_output = os.environ.get("GITHUB_OUTPUT")
    if github_output:
        with open(github_output, "a", encoding="utf-8") as handle:
            handle.write(f"blocking={'true' if blocking else 'false'}\n")


if __name__ == "__main__":
    main()
