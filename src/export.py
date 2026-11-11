from pathlib import Path
from .tables import ROOT, read_table, write_table
from .summary import group_rows

def export_runs(run_directory, output_directory):
    source, target = Path(run_directory), Path(output_directory)
    original = (ROOT / "data/raw").resolve()
    if target.resolve() == original or original in target.resolve().parents:
        raise ValueError("export to a fresh directory, not packaged data")
    target.mkdir(parents=True, exist_ok=True)
    if any(target.iterdir()):
        raise ValueError("output directory must be empty")
    names = ["main", "index_online", "parallel", "ablation", "gap", "index_build", "index_filter", "online_filter", "correctness"]
    tables = {}
    for name in names:
        path = source / name / "raw.tsv"
        if not path.exists():
            raise FileNotFoundError("missing experiment output: " + str(path))
        tables[name] = read_table(path)
        for row in tables[name]:
            if row.get("status") and row["status"] not in ("OK","EMPTY","PASS","TIMEOUT","TIMEOUT_SOFT","TIMEOUT_HARD"):
                raise ValueError("failed configuration in output: " + name)
    main = {(r["graph"],r["gap"],r["query_id"],r["method"]):r for r in tables["main"]}
    probes = {(r["graph"],r["gap"],r["query_id"],r["method"]):r for r in read_table(source/"response/raw.tsv")}
    modes = read_table(ROOT/"data/raw/response.tsv")
    measurements = []
    for mode in modes:
        key=tuple(mode[k] for k in ["graph","gap","query_id","method"])
        full=main[key];probe=probes[key] if mode["measurement_mode"]=="response_only" else full
        row={k:mode[k] for k in ["graph","dataset","gap","query_id","method","query_seed","source_internal_id","target_internal_id","x1","x2","delta_min","delta_max","measurement_mode"]}
        row.update(full_status=full["status"],full_answers=full["path_count"],full_query_ms_raw=full["online_total_ms"],
                   response_run_status=probe.get("response_run_status",probe.get("status","")),response_target=probe.get("response_target","1000"),
                   response_1000_ms_raw=probe.get("response_1000_ms_raw",probe.get("response_1000_ms","")),
                   response_run_online_ms_raw=probe.get("response_run_online_ms_raw",probe.get("online_total_ms","")),
                   response_status=probe.get("response_status",""))
        measurements.append(row)
    tables["response"]=measurements
    from .tables import load_json
    build_rows=[]
    for path in (source/"artifacts/index").glob("*.json"):
        saved=load_json(path)
        if not isinstance(saved,dict) or "measurement" not in saved:continue
        graph=path.stem;measurement=saved["measurement"];identity=saved["identity"]
        topology=load_json(source/"cache/index"/(graph+".topology.json"))["measurement"]
        sample=next(r for r in tables["index_online"] if r["dataset"]==graph)
        build_rows.append(dict(dataset=graph,delta_min=identity["delta_min"],delta_max=identity["delta_max"],
                               **{k:measurement.get(k,"") for k in ["index_build_ms","index_algorithm_build_ms","index_bytes","index_serialized_bytes"]},
                               **{k:topology.get(k,"") for k in ["topology_build_ms","topology_index_bytes","topology_serialized_bytes"]},
                               graph_fingerprint=sample.get("graph_fingerprint",""),graph_vertices=sample.get("graph_vertex_count",""),
                               graph_edges=sample.get("graph_edge_count",""),time_span=str(int(sample["graph_time_span"])-1) if sample.get("graph_time_span") else "",source_file=""))
    tables["index_online_build"]=build_rows
    from .tables import load_json
    schemas=load_json(ROOT/"configs/schemas.json")
    for name,rows in tables.items():
        if len(rows)!=schemas[name]["rows"]:
            raise ValueError("incomplete output: "+name)
    for name,rows in tables.items():write_table(target/(name+".tsv"),rows,schemas[name]["fields"])
    print("Export finished.")
