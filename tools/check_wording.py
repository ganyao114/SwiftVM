#!/usr/bin/env python3
"""Check maintained project text and filenames against the naming rule."""

from pathlib import Path
import re
import subprocess


ROOT = Path(__file__).resolve().parents[1]
# Fragments keep the rule's own source free of the rejected spellings.
WORDS = ("material" + "ized", "norm" + "alize", "pl" + "an",
         "pr" + "obe", "snap" + "shot", "iden" + "tity", "pl" + "ain")
REJECTED = re.compile("|".join(WORDS), re.IGNORECASE)


def project_files():
    output = subprocess.check_output(
        ["git", "ls-files", "-c", "-o", "--exclude-standard", "-z"], cwd=ROOT)
    names = set(output.decode().split("\0")) - {""}
    # Local analysis tools are maintained here even when excluded from Git.
    for suffix in ("*.py", "*.sh"):
        names.update(str(p.relative_to(ROOT)) for p in (ROOT / "tools").rglob(suffix))
    for name in sorted(names):
        path = ROOT / name
        if (name.startswith("build-master/") or "externals" in path.parts or
                ".venv" in path.parts or path.is_symlink() or not path.is_file() or
                path.suffix in {".png", ".ppm", ".jpg", ".o", ".a", ".so"}):
            continue
        yield name, path


def main():
    failures = []
    count = 0
    for name, path in project_files():
        try:
            text = path.read_text()
        except UnicodeDecodeError:
            continue
        count += 1
        if REJECTED.search(name):
            failures.append(f"{name}: filename violates naming rule")
        for line, content in enumerate(text.splitlines(), 1):
            if REJECTED.search(content):
                failures.append(f"{name}:{line}: text violates naming rule")
    if failures:
        print("\n".join(failures))
        return 1
    print(f"Naming check passed: {count} project text files")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
