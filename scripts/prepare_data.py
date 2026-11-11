#!/usr/bin/env python3
import argparse
import gzip
import hashlib
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from src.tables import read_table

def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(4 * 1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()

def main():
    parser = argparse.ArgumentParser(description="Verify and stage externally supplied graph inputs")
    parser.add_argument("--out", required=True)
    parser.add_argument("--graph")
    parser.add_argument("--input-dir", type=Path, help="directory containing prepared text graphs")
    parser.add_argument("--archive-dir", type=Path, help="directory containing the prepared gzip archives")
    parser.add_argument("--manifest", type=Path, default=ROOT / "data/graph_manifest.tsv")
    parser.add_argument("--configure", action="store_true")
    args = parser.parse_args()
    if args.input_dir and args.archive_dir:
        parser.error("choose --input-dir or --archive-dir")
    destination = Path(args.out).resolve()
    destination.mkdir(parents=True, exist_ok=True)
    manifest = read_table(args.manifest)
    if args.graph and args.graph not in {row["graph"] for row in manifest}:
        parser.error("unknown graph: " + args.graph)
    if args.configure and args.graph:
        parser.error("--configure requires all twelve inputs")
    for row in manifest:
        if args.graph and row["graph"] != args.graph:
            continue
        output = destination / row["filename"]
        if output.exists():
            if sha256(output) != row["raw_sha256"]:
                raise RuntimeError("existing input differs: " + str(output))
        else:
            source = (args.input_dir / row["filename"]) if args.input_dir else None
            archive = (args.archive_dir / Path(row["archive"]).name) if args.archive_dir else ROOT / row["archive"]
            path = source if source is not None else archive
            if not path.is_file():
                raise FileNotFoundError("Graph inputs are not bundled. See docs/DATASETS.md; supply --input-dir or --archive-dir. Missing: " + str(path))
            expected = row["raw_sha256"] if source is not None else row["archive_sha256"]
            if sha256(path) != expected:
                raise RuntimeError("source checksum differs: " + str(path))
            temporary = output.with_suffix(output.suffix + ".tmp")
            try:
                opener = path.open("rb") if source is not None else gzip.open(path, "rb")
                with opener as incoming, temporary.open("wb") as target:
                    shutil.copyfileobj(incoming, target, 4 * 1024 * 1024)
                if sha256(temporary) != row["raw_sha256"]:
                    raise RuntimeError("prepared input differs: " + str(output))
                temporary.replace(output)
            except BaseException:
                if temporary.exists():
                    temporary.unlink()
                raise
        print(row["graph"], "verified", flush=True)
    if args.configure:
        subprocess.run([sys.executable, str(ROOT / "run.py"), "configure", "--data-dir", str(destination)], check=True)

if __name__ == "__main__":
    main()
