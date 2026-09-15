from __future__ import annotations

import argparse
from pathlib import Path
import shutil
import subprocess

SOURCES = (
    "vita3k/main.cpp",
    "vita3k/gui-qt/src",
    "vita3k/gui-qt/include",
)

# Debian ships lupdate outside PATH, so the usual install prefixes are searched as well.
LUPDATE_PREFIXES = ("/usr/lib", "/usr/lib64", "/usr/local/lib", "/usr/share")


def find_lupdate() -> str:
    for name in ("lupdate", "lupdate-qt6", "lupdate6"):
        found = shutil.which(name)
        if found:
            return found

    for prefix in LUPDATE_PREFIXES:
        for pattern in ("qt6/bin/lupdate", "*/qt6/bin/lupdate"):
            for candidate in sorted(Path(prefix).glob(pattern)):
                if candidate.is_file():
                    return str(candidate)

    raise RuntimeError("lupdate was not found. Install the Qt linguist tools or pass --lupdate.")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", default=str(Path(__file__).resolve().parents[2]))
    parser.add_argument("--output", default="template/vita3k_template.ts")
    parser.add_argument("--lupdate")
    args = parser.parse_args()

    repo_root = Path(args.repo_root).resolve()
    output = (repo_root / args.output).resolve()
    output.parent.mkdir(parents=True, exist_ok=True)

    command = [
        args.lupdate or find_lupdate(),
        "-no-obsolete",
        "-recursive",
        *SOURCES,
        "-ts",
        str(output),
    ]

    print(" ".join(command))
    subprocess.run(command, cwd=repo_root, check=True)


if __name__ == "__main__":
    main()
