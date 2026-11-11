#!/usr/bin/env python3
import argparse
import csv
import hashlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

def main():
    parser = argparse.ArgumentParser(description="Check files against the release manifest")
    parser.add_argument("--include-graphs", action="store_true")
    args = parser.parse_args()
    checked = 0
    with (ROOT / "SHA256SUMS.tsv").open(encoding="utf-8") as stream:
        for row in csv.DictReader(stream, delimiter="\t"):
            if row["file"].startswith("data/graph_archives/") and not args.include_graphs:
                continue
            path = ROOT / row["file"]
            digest = hashlib.sha256()
            with path.open("rb") as source:
                for block in iter(lambda: source.read(4 * 1024 * 1024), b""):
                    digest.update(block)
            if digest.hexdigest() != row["sha256"]:
                raise RuntimeError("checksum mismatch: " + row["file"])
            checked += 1
    print("Verified", checked, "release files")

if __name__ == "__main__":
    main()
