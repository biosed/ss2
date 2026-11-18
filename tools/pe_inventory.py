#!/usr/bin/env python3
"""Print redistributable PE metadata for an external, unpacked game directory."""
import argparse
import hashlib
from pathlib import Path
import re
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("root", type=Path)
    root = parser.parse_args().root.resolve(strict=True)
    files = sorted(path for path in root.rglob("*")
                   if path.is_file() and path.suffix.lower() in {".exe", ".dll"})
    if not files:
        parser.error("no executable or DLL found")
    print("path\tsha256\tmachine\timage_base\tentry_rva\tstatic_imports")
    for path in files:
        report = subprocess.check_output(
            ["llvm-readobj", "--file-headers", "--coff-imports", str(path)], text=True)
        fields = [re.search(r"^  " + key + r": (\S+)", report, re.MULTILINE)[1]
                  for key in ("Machine", "ImageBase", "AddressOfEntryPoint")]
        imports = re.findall(r"^  Name: (.+)$", report, re.MULTILINE)
        with path.open("rb") as stream:
            digest = hashlib.file_digest(stream, "sha256").hexdigest()
        print(path.relative_to(root), digest, *fields, ",".join(imports) or "-", sep="\t")


if __name__ == "__main__":
    main()
