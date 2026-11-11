import hashlib
import json
import signal
from contextlib import ExitStack
from pathlib import Path
from .benchmarks import Benchmark, query_options, file_hash
from .journal import FileLock, ResultJournal
from .plans import case_key
from .processes import QueryProcess
from .summary import group_rows
from .tables import ROOT, load_json, read_table

def replay_measurements(mode, registry, output_root, graph=None, retry_interrupted=False,
                        query_file=None, build_file=None, limit=None):
    experiment = {"grid":"index_filter", "online":"online_filter", "gap":"gap", "correctness":"correctness"}[mode]
    benchmark = Benchmark(experiment, registry, output_root)
    queries = read_table(query_file or ROOT/"queries"/(experiment+".tsv"))
    builds = read_table(build_file or ROOT/"queries/index_build.tsv") if mode == "grid" else []
    if graph:
        queries = [row for row in queries if row["graph"] == graph]
        builds = [row for row in builds if row["graph"] == graph]
    if limit is not None:
        queries = queries[:limit]
    if not queries:
        raise ValueError("empty recorded-session plan")
    schema = load_json(ROOT/"configs/schemas.json")
    source = b"".join(path.read_bytes() for path in sorted((ROOT/"src").glob("*.py")))
    manifest = dict(process_model="recorded_graph_resident_session", worker=file_hash(benchmark.worker),
                    registry=file_hash(registry), source=hashlib.sha256(source).hexdigest(),
                    queries=hashlib.sha256(json.dumps(queries,sort_keys=True).encode()).hexdigest(),
                    builds=hashlib.sha256(json.dumps(builds,sort_keys=True).encode()).hexdigest(),
                    graphs={name:dict(path=str(Path(row["graph_file"]).resolve()),size=Path(row["graph_file"]).stat().st_size,
                                    mtime=Path(row["graph_file"]).stat().st_mtime_ns)
                            for name,row in benchmark.registry.items() if name in {q["graph"] for q in queries}})
    def stop(signum, frame):
        raise KeyboardInterrupt("termination requested")
    previous = signal.signal(signal.SIGTERM, stop)
    try:
        with ExitStack() as stack:
            stack.enter_context(FileLock(ROOT/".benchmark.lock"))
            query_book=stack.enter_context(ResultJournal(Path(output_root)/experiment,manifest,schema[experiment]["fields"],retry_interrupted))
            build_book=stack.enter_context(ResultJournal(Path(output_root)/"index_build",manifest,schema["index_build"]["fields"],retry_interrupted)) if builds else None
            def recorded(book, job, row):
                record=book.record(case_key(job,row))
                if record and record["status"] not in ("OK","PASS","TIMEOUT","TIMEOUT_SOFT","TIMEOUT_HARD"):
                    raise RuntimeError("previous failed configuration requires inspection")
                return record
            def measure(process, book, job, row, action, values, timeout):
                key=case_key(job,row)
                book.begin(key,dict(action=action,values={k:str(v) for k,v in values.items()}))
                result=process.request(action,values,timeout)
                benchmark.check_answer(row,result)
                projected=benchmark.project(row,result)
                book.commit(key,result,projected)
                print(job,row["graph"],row.get("query_id",row.get("index_type","")),result["status"],flush=True)
                if result["status"] not in ("OK","PASS","TIMEOUT","TIMEOUT_SOFT"):
                    raise RuntimeError("recorded configuration failed or worker terminated")
                return result
            for (name,), graph_queries in group_rows(queries,["graph"]).items():
                if all(recorded(query_book,experiment,q) for q in graph_queries):
                    continue
                cache=benchmark.graph_cache(name)
                with QueryProcess(benchmark.worker,cache,benchmark.error_file) as process:
                    if mode != "grid":
                        for query in graph_queries:
                            if recorded(query_book,experiment,query):
                                continue
                            values=query_options(query)
                            values["soft-timeout"] = benchmark.protocol["soft_seconds"]
                            action = {"online":"online-filter", "gap":"sweep", "correctness":"correct"}[mode]
                            if mode == "correctness":
                                values.update({"max-sequences":benchmark.protocol["max_sequences"],
                                               "sequence-dir":Path(output_root)/"sequences"/name/query["query_id"]})
                            measure(process,query_book,experiment,query,action,values,benchmark.protocol["hard_seconds"])
                        continue
                    for (gap_group,), group_queries in group_rows(graph_queries,["gap_group"]).items():
                        if all(recorded(query_book,experiment,q) for q in group_queries):
                            continue
                        group_builds=[b for b in builds if b["graph"]==name and b["gap_group"]==gap_group]
                        if [b["index_type"] for b in group_builds] != ["PoolIndex","L1","L2"]:
                            raise ValueError("recorded build order differs")
                        files={}
                        for build in group_builds:
                            path=benchmark.artifact_root/name/gap_group/(build["index_type"]+".bin")
                            path.parent.mkdir(parents=True,exist_ok=True);files[build["index_type"]]=path
                            if recorded(build_book,"index_build",build):
                                if not path.exists():
                                    raise RuntimeError("restore the index artifact needed by the unfinished group: "+str(path))
                                continue
                            measure(process,build_book,"index_build",build,"build-index",
                                    {"index-type":build["index_type"],"index-file":path,"delta-min":build["d_min"],
                                     "delta-max":build["d_max"],"soft-timeout":21600,"max-index-bytes":0},21630)
                        for kind in ["PoolIndex","L1","L2"]:
                            selected=[q for q in group_queries if q["index_type"]==kind]
                            if all(recorded(query_book,experiment,q) for q in selected):
                                continue
                            benchmark.require_ok(process.request("load-index",{"index-type":kind,"index-file":files[kind]},1200))
                            for query in selected:
                                if recorded(query_book,experiment,query):
                                    continue
                                values=query_options(query);values.update({"index-type":kind,"soft-timeout":120})
                                measure(process,query_book,experiment,query,"index-query",values,150)
                        benchmark.require_ok(process.request("clear-index",{},30))
                        for build in group_builds:
                            benchmark.release_layout(build)
    finally:
        signal.signal(signal.SIGTERM, previous)
