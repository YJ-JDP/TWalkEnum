import json
from collections import Counter
from .tables import ROOT, load_json, read_table, write_table, integer
from .plans import make_plan
from .summary import group_rows, response_statistics

def validate_data():
    (ROOT / "validation").mkdir(parents=True, exist_ok=True)
    schemas = load_json(ROOT / "configs/schemas.json")
    checks = []
    for name, schema in schemas.items():
        rows = read_table(ROOT / schema["file"])
        if len(rows) != schema["rows"] or list(rows[0]) != schema["fields"]:
            raise ValueError("table contract differs: " + name)
        checks.append(dict(experiment=name, rows=len(rows), status="PASS"))
    for name, fields, size in [("main", ["graph","gap","method"],500),
                               ("response",["graph","gap","method"],500),
                               ("index_online",["dataset","method"],500),
                               ("parallel",["graph","workers"],50)]:
        rows = read_table(ROOT / "data/raw" / (name+".tsv"))
        if any(len(group)!=size for group in group_rows(rows,fields).values()):
            raise ValueError("unexpected cohort size: " + name)
        keys=[tuple(row.get(k,"") for k in fields+["query_id"]) for row in rows]
        if len(keys)!=len(set(keys)):
            raise ValueError("duplicate cases: " + name)
    for name in ["main","index_online"]:
        rows=read_table(ROOT/"data/raw"/(name+".tsv"))
        fields=["graph","gap","query_id"] if name=="main" else ["dataset","query_id"]
        for key, group in group_rows(rows,fields).items():
            params=["source_internal_id","target_internal_id","x1","x2","delta_min","delta_max","query_seed"]
            if len({tuple(row[k] for k in params) for row in group})!=1:
                raise ValueError("algorithm query mismatch: "+str(key))
            counts={integer(row["path_count"]) for row in group if row["status"]=="OK"}
            if len(counts)>1:raise ValueError("complete answer mismatch: "+str(key))
    response=response_statistics(read_table(ROOT/"data/raw/response.tsv"))
    if len(response)!=36000:raise ValueError("incomplete response collection")
    planned={name:len(make_plan(name)) for name in load_json(ROOT/"configs/protocols.json")}
    write_table(ROOT/"validation/data_validation.tsv",checks)
    report=dict(status="PASS",table_rows={r["experiment"]:r["rows"] for r in checks},planned_cases=planned,
                response_rules=dict(Counter(r["rule"] for r in response)),
                note="Response runs cover the selected independent probes; other thresholds come from main runs.")
    (ROOT/"validation/data_validation.json").write_text(json.dumps(report,indent=2),encoding="utf-8")
    return report
