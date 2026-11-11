#!/usr/bin/env python3
import argparse
import json
import os
import subprocess
from pathlib import Path
from src.tables import ROOT, load_json, read_table, write_table

BACKENDS = ["main", "index", "parallel", "gap", "filter", "correctness", "ablation"]

def configure_graphs(directory):
    directory = Path(directory).resolve()
    rows = []
    for dataset in read_table(ROOT / "configs/datasets.tsv"):
        path = directory / dataset["filename"]
        if not path.is_file():
            raise FileNotFoundError(path)
        rows.append(dict(graph=dataset["name"], dataset=Path(dataset["filename"]).stem,
                         graph_file=str(path), vertices=dataset["vertices"], events=dataset["events"],
                         delimiter="space", src_column="0", dst_column="1", time_column="2", id_column="-1", skip_header="0"))
    write_table(ROOT / "configs/graphs.tsv", rows)
    print("Configured", len(rows), "graphs; input files were not modified.")

def build_backends(args):
    selected = BACKENDS if args.backend == "all" else [args.backend]
    for name in selected:
        source = ROOT / "kernels" / name
        command = [args.cmake, "-S", str(source), "-B", str(source / "build"), "-DCMAKE_BUILD_TYPE=Release"]
        if args.generator:
            command += ["-G", args.generator]
        if args.compiler:
            command += ["-DCMAKE_CXX_COMPILER=" + args.compiler]
        if args.make_program:
            command += ["-DCMAKE_MAKE_PROGRAM=" + args.make_program]
        subprocess.run(command, check=True)
        subprocess.run([args.cmake, "--build", str(source / "build"), "-j", str(args.jobs)], check=True)

def main():
    parser = argparse.ArgumentParser(description="Fixed-query TWalkEnum experiments")
    commands = parser.add_subparsers(dest="command", required=True)
    config = commands.add_parser("configure")
    config.add_argument("--data-dir", required=True)
    build = commands.add_parser("build")
    build.add_argument("--backend", choices=["all"] + BACKENDS, default="all")
    build.add_argument("--cmake", default="cmake")
    build.add_argument("--compiler")
    build.add_argument("--generator")
    build.add_argument("--make-program")
    build.add_argument("--jobs", type=int, default=2)
    run = commands.add_parser("run")
    run.add_argument("experiment", choices=list(load_json(ROOT / "configs/protocols.json")))
    run.add_argument("--graphs", default=str(ROOT / "configs/graphs.tsv"))
    run.add_argument("--graph")
    run.add_argument("--queries")
    run.add_argument("--out", default=str(ROOT / "runs"))
    run.add_argument("--worker")
    run.add_argument("--limit", type=int, help="first N measured configurations")
    run.add_argument("--retry-interrupted", action="store_true")
    run.add_argument("--dry-run", action="store_true")
    commands.add_parser("validate")
    summary = commands.add_parser("summarize")
    summary.add_argument("--data", default=str(ROOT / "data/raw"))
    summary.add_argument("--out", default=str(ROOT / "results/paper_tables"))
    export = commands.add_parser("export")
    export.add_argument("--runs", default=str(ROOT / "runs"))
    export.add_argument("--out", required=True)
    args = parser.parse_args()
    if args.command == "configure":
        configure_graphs(args.data_dir)
    elif args.command == "build":
        build_backends(args)
    elif args.command == "validate":
        from src.validate import validate_data
        print(json.dumps(validate_data(), indent=2))
    elif args.command == "summarize":
        from src.summary import summarize
        summarize(args.data, args.out)
        print("Wrote summaries to", args.out)
    elif args.command == "export":
        from src.export import export_runs
        export_runs(args.runs, args.out)
    else:
        if args.limit is not None and args.limit < 1:
            parser.error("--limit must be positive")
        target = Path(args.out).resolve()
        data = (ROOT / "data").resolve()
        if target == data or data in target.parents:
            parser.error("new runs must not overwrite packaged data")
        from src.runner import run_experiment
        run_experiment(args.experiment, args.graphs, args.out, args.graph, args.queries,
                       args.limit, args.retry_interrupted, args.dry_run, args.worker)

if __name__ == "__main__":
    main()
