#!/usr/bin/env python3
"""Documentation / repo consistency checks.

Run from anywhere:

    python3 tools/check_docs.py

Checks, over every tracked-style text file (managed_components/, build/,
.venv/, __pycache__/ and other generated dirs excluded):

  1. every `notes/<file>.md` reference resolves;
  2. every `examples/<name>/README.md` reference resolves;
  3. the app tables in README.md / examples/README.md match the directories
     that actually exist under examples/;
  4. markdown code fences are balanced;
  5. no `-D...API_KEY` / `-D...PASSWORD` / `-D...SSID` on a command line
     (secrets belong in the environment / secrets.local, never in a build
     cache or shell history).

Exit status is 1 when anything is found, so it can gate a commit/CI.
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

EXCLUDE_PARTS = {
    ".git", "build", "managed_components", ".venv", "__pycache__",
    ".Trash-0", ".workbuddy", "scripts",
}
# `scripts/` is excluded only for its generated venv/audio, but its sources are
# worth checking; handle that by excluding the specific subpaths below.
EXCLUDE_SUBPATHS = {
    ROOT / "scripts" / ".venv",
    ROOT / "scripts" / "__pycache__",
    ROOT / "scripts" / "assets",
}
TEXT_SUFFIXES = {".md", ".c", ".h", ".txt", ".yml", ".yaml", ".defaults",
                 ".sh", ".py", ".csv", ".lock"}

SELF = Path(__file__).resolve()

ISSUES: list[tuple[str, int, str]] = []


def add(path: Path, line: int, msg: str) -> None:
    ISSUES.append((str(path.relative_to(ROOT)), line, msg))


def iter_files():
    for path in sorted(ROOT.rglob("*")):
        if not path.is_file() or path.resolve() == SELF:
            continue
        rel = path.relative_to(ROOT)
        if any(part in EXCLUDE_PARTS for part in rel.parts):
            continue
        if any(path.resolve() == p or p in path.resolve().parents
               for p in EXCLUDE_SUBPATHS):
            continue
        if path.suffix in TEXT_SUFFIXES:
            yield path


def line_of(text: str, pos: int) -> int:
    return text.count("\n", 0, pos) + 1


def line_text(text: str, pos: int) -> str:
    start = text.rfind("\n", 0, pos) + 1
    end = text.find("\n", pos)
    if end == -1:
        end = len(text)
    return text[start:end]


def ignored(text: str, pos: int) -> bool:
    """A line annotated with `docs-ignore` is skipped (historical notes that
    mention a since-deleted path, for example)."""
    return "docs-ignore" in line_text(text, pos)


def check_notes_and_examples_refs(files):
    notes_re = re.compile(r"notes/([A-Za-z0-9._/-]+\.md)")
    ex_re = re.compile(r"examples/([A-Za-z0-9_]+)/README\.md")
    for path in files:
        text = path.read_text(encoding="utf-8", errors="replace")
        for m in notes_re.finditer(text):
            if ignored(text, m.start()):
                continue
            if not (ROOT / "notes" / m.group(1)).exists():
                add(path, line_of(text, m.start()),
                    f"dangling notes reference: {m.group(0)}")
        for m in ex_re.finditer(text):
            if ignored(text, m.start()):
                continue
            if not (ROOT / "examples" / m.group(1) / "README.md").exists():
                add(path, line_of(text, m.start()),
                    f"dangling examples reference: {m.group(0)}")


def check_app_tables(files):
    dirs = {p.name for p in (ROOT / "examples").iterdir()
            if p.is_dir() and (p / "CMakeLists.txt").exists()}
    for table in (ROOT / "README.md", ROOT / "examples" / "README.md"):
        if not table.exists():
            continue
        text = table.read_text(encoding="utf-8", errors="replace")
        listed = set(re.findall(r"^\|\s*`([0-9]{2}_[A-Za-z0-9_]+)`",
                                text, re.MULTILINE))
        for missing in sorted(dirs - listed):
            add(table, 0, f"app directory not in the table: {missing}")
        for unknown in sorted(listed - dirs):
            add(table, 0, f"table entry has no directory: {unknown}")


def check_code_fences(files):
    for path in files:
        if path.suffix != ".md":
            continue
        text = path.read_text(encoding="utf-8", errors="replace")
        fences = re.findall(r"^\s*```", text, re.MULTILINE)
        if len(fences) % 2 != 0:
            add(path, 0, f"unbalanced code fences ({len(fences)} markers)")


def check_secret_flags(files):
    flag_re = re.compile(r"-D[A-Za-z0-9_]*(?:API_KEY|PASSWORD|SSID)")
    for path in files:
        text = path.read_text(encoding="utf-8", errors="replace")
        for m in flag_re.finditer(text):
            add(path, line_of(text, m.start()),
                f"secret passed as a CMake -D flag: {m.group(0)}")


def main() -> int:
    files = list(iter_files())
    check_notes_and_examples_refs(files)
    check_app_tables(files)
    check_code_fences(files)
    check_secret_flags(files)

    for rel, line, msg in ISSUES:
        loc = f"{rel}:{line}" if line else rel
        print(f"{loc}: {msg}")
    print(f"check_docs: {len(ISSUES)} issue(s)")
    return 1 if ISSUES else 0


if __name__ == "__main__":
    sys.exit(main())
