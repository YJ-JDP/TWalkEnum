import json
import random
from .tables import ROOT, read_table, load_json

MAIN_METHODS = ["TWalkEnum", "TSPG-style", "PathEnum-DFS-style", "CPE-startup-style"]
MAIN_SHUFFLE_BASE = ["PathEnum-DFS-style", "CPE-startup-style", "TSPG-style", "TWalkEnum"]
RESPONSE_SHUFFLE_BASE = ["TWalkEnum", "PathEnum-DFS-style", "TSPG-style"]
INDEX_METHODS = ["PoolIndex+TWalkEnum", "TWalkEnum", "Indexed-PathEnum-DFS-style", "Indexed-CPE-startup-style", "Indexed-TSPG-style"]

def make_plan(experiment, query_file=None, graph=None):
    if experiment == "ablation":
        from .ablation import make_cases
        queries = read_table(query_file or ROOT / "queries/ablation.tsv")
        if graph:
            queries = [q for q in queries if q["graph"] == graph]
        if not queries:
            raise ValueError("no selected cases")
        return make_cases(queries)
    name = experiment
    queries = read_table(query_file or ROOT / "queries" / (name + ".tsv"))
    protocol = load_json(ROOT / "configs/protocols.json")[experiment]
    cases = []
    response_groups = {}
    if experiment == "response":
        for query in queries:
            response_groups.setdefault((query["graph"], query["gap"], query["query_id"]), []).append(query)
    for query in queries:
        if graph and query["graph"] != graph:
            continue
        if experiment == "main":
            if query.get("method_order"):
                methods = query["method_order"].split("|")
                if len(methods) != 4 or set(methods) != set(MAIN_METHODS):
                    raise ValueError("invalid main method order")
            else:
                methods = list(MAIN_SHUFFLE_BASE)
                random.Random(int(query["query_seed"])).shuffle(methods)
            cases.extend(dict(query, method=method) for method in methods)
        elif experiment == "response":
            group = response_groups[(query["graph"], query["gap"], query["query_id"])]
            if query is not group[0]:
                continue
            by_method = {row["method"]: row for row in group}
            methods = [method for method in RESPONSE_SHUFFLE_BASE if method in by_method]
            random.Random(int(query["query_seed"])).shuffle(methods)
            cases.extend(dict(by_method[method]) for method in methods)
        elif experiment == "index_online":
            cases.extend(dict(query, method=method) for method in INDEX_METHODS)
        elif experiment == "parallel":
            cases.extend(dict(query, variant="full", window_mode="gb", window_seed="0", workers=str(workers))
                         for workers in protocol["threads"])
        else:
            cases.append(dict(query))
    keys = [case_key(experiment, case) for case in cases]
    if len(keys) != len(set(keys)):
        raise ValueError("duplicate cases in input plan")
    if not cases:
        raise ValueError("no selected cases")
    return cases

def case_key(experiment, case):
    fields = ["graph", "gap", "bucket", "gap_group", "query_id", "method", "index_type",
              "variant", "window_mode", "window_seed", "workers"]
    return json.dumps([experiment] + [case.get(field, "") for field in fields], separators=(",", ":"))
