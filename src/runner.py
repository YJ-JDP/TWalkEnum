import hashlib
import json
import os
import signal
from pathlib import Path
from .tables import ROOT, load_json, read_table
from .plans import make_plan, case_key
from .benchmarks import Benchmark, file_hash
from .journal import ResultJournal, FileLock

ALLOWED_STATUS = {"OK", "EMPTY", "PASS", "TIMEOUT", "TIMEOUT_SOFT", "TIMEOUT_HARD", "RESPONSE_TARGET_REACHED", "MEMORY_LIMIT", "TIMEOUT_BUILD"}

def run_experiment(experiment, registry, output_root, graph=None, query_file=None,
                   limit=None, retry_interrupted=False, dry_run=False, worker=None):
    if experiment == "ablation":
        from .ablation import run_experiment as run_ablation
        return run_ablation(registry, output_root, graph, query_file, limit,
                            retry_interrupted, dry_run, worker)
    cases = make_plan(experiment, query_file, graph)
    if limit is not None:
        cases = cases[:limit]
    protocol = load_json(ROOT / "configs/protocols.json")[experiment]
    if dry_run:
        print(json.dumps(dict(experiment=experiment, cases=len(cases), protocol=protocol,
                              first_case=cases[0], fresh_process_per_case=True), indent=2))
        return
    benchmark = Benchmark(experiment, registry, output_root, worker)
    graph_rows = {r["graph"]: r for r in read_table(registry)}
    graph_identity = {}
    for name in {case["graph"] for case in cases}:
        path = Path(graph_rows[name]["graph_file"])
        stat = path.stat()
        graph_identity[name] = dict(path=str(path.resolve()), size=stat.st_size, mtime=stat.st_mtime_ns)
    driver_source = b"".join(path.read_bytes() for path in sorted((ROOT / "src").glob("*.py"))) + (ROOT / "run.py").read_bytes()
    manifest = dict(experiment=experiment, protocol=protocol, worker_sha256=file_hash(benchmark.worker),
                    driver_sha256=hashlib.sha256(driver_source).hexdigest(),
                    cases_sha256=hashlib.sha256(json.dumps(cases, sort_keys=True).encode()).hexdigest(),
                    registry_sha256=file_hash(registry), graphs=graph_identity,
                    process_model="one measured configuration per child process")
    def release_finished_layout(case, next_case):
        fields = ("graph", "gap_group", "index_type")
        boundary = next_case is None or any(case.get(k) != next_case.get(k) for k in fields)
        if experiment == "index_build" or (experiment == "index_filter" and boundary):
            benchmark.release_layout(case)

    schemas = load_json(ROOT / "configs/schemas.json")
    fields = schemas[experiment]["fields"]
    def interrupted(signum, frame):
        raise KeyboardInterrupt("termination requested")
    execution_lock = FileLock(ROOT / ".benchmark.lock")
    previous = signal.signal(signal.SIGTERM, interrupted)
    try:
        with ResultJournal(Path(output_root) / experiment, manifest, fields, retry_interrupted) as journal:
            for position, case in enumerate(cases, 1):
                key = case_key(experiment, case)
                if journal.contains(key):
                    if journal.record(key).get("status") not in ALLOWED_STATUS:
                        raise RuntimeError("previous failed case requires inspection: " + key)
                    release_finished_layout(case, cases[position] if position < len(cases) else None)
                    continue
                journal.begin(key, case)
                result = benchmark.execute(case)
                benchmark.check_answer(case, result)
                row = benchmark.project(case, result)
                journal.commit(key, result, row)
                release_finished_layout(case, cases[position] if position < len(cases) else None)
                print("%s %d/%d %s %s %s pid=%s" % (experiment, position, len(cases), case["graph"],
                      case.get("query_id", case.get("gap_group", "")), result["status"], result.get("_pid", "")), flush=True)
                if result["status"] not in ALLOWED_STATUS:
                    raise RuntimeError("recorded failing case: " + key + "; inspect journal.sqlite3")
    finally:
        signal.signal(signal.SIGTERM, previous)
        execution_lock.close()
