import hashlib
import json
import os
from pathlib import Path
from .tables import ROOT, load_json, read_table, write_table, integer
from .processes import run_process, QueryProcess

EXECUTABLES = {"main": "main_worker", "index": "index_worker", "parallel": "parallel_worker",
               "gap": "gap_worker", "filter": "filter_worker", "correctness": "correctness_worker"}

def file_hash(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()

def options(values):
    return [item for key, value in values.items() for item in ("--" + key, str(value))]

def query_options(query):
    values = {}
    for flag, candidates in {"source": ["source_internal_id", "src_internal", "src"],
                             "target": ["target_internal_id", "dst_internal", "dst"],
                             "x1": ["x1"], "x2": ["x2"], "delta-min": ["delta_min", "d_min"],
                             "delta-max": ["delta_max", "d_max"], "query-id": ["query_id"],
                             "query-seed": ["query_seed"]}.items():
        for field in candidates:
            if query.get(field, "") != "":
                values[flag] = query[field]
                break
    return values

class Benchmark:
    def __init__(self, experiment, registry, run_root, worker=None):
        self.experiment = experiment
        self.protocol = load_json(ROOT / "configs/protocols.json")[experiment]
        self.backend = self.protocol["backend"]
        self.worker = Path(worker) if worker else ROOT / "kernels" / self.backend / "build/bin" / EXECUTABLES[self.backend]
        if os.name == "nt" and not self.worker.is_file():
            self.worker = self.worker.with_suffix(".exe")
        if not self.worker.is_file():
            raise FileNotFoundError("build backend first: " + str(self.worker))
        self.registry = {row["graph"]: row for row in read_table(registry)}
        self.run_root = Path(run_root).resolve()
        self.cache_root = self.run_root / "cache" / self.backend
        self.artifact_root = self.run_root / "artifacts" / self.backend
        self.error_file = self.run_root / (experiment + "_worker.stderr.log")
        self.run_root.mkdir(parents=True, exist_ok=True)
        self.worker_hash = file_hash(self.worker)

    def graph_cache(self, graph):
        info = self.registry[graph]
        source = Path(info["graph_file"])
        stat = source.stat()
        identity = dict(source=str(source.resolve()), size=stat.st_size, mtime=stat.st_mtime_ns,
                        worker=self.worker_hash,
                        loader={k: info.get(k, "") for k in ["delimiter", "src_column", "dst_column", "time_column", "id_column", "skip_header"]})
        cache = self.cache_root / (graph + ".bin")
        meta = cache.with_suffix(".json")
        if not cache.exists() or not meta.exists() or load_json(meta) != identity:
            cache.parent.mkdir(parents=True, exist_ok=True)
            values = {"action": "cache-graph", "graph-file": source, "graph-cache": cache}
            for key in ["delimiter", "src_column", "dst_column", "time_column", "id_column", "skip_header"]:
                if info.get(key, "") != "":
                    values[key.replace("_", "-")] = info[key]
            result = run_process([self.worker] + options(values), 18000, allow_empty=True)
            self.require_ok(result)
            for measured, expected in [("graph_vertex_count", "vertices"), ("graph_edge_count", "events")]:
                if measured in result and info.get(expected) and result[measured] != info[expected]:
                    raise RuntimeError("graph statistics differ: " + graph + "/" + expected)
            meta.write_text(json.dumps(identity, indent=2), encoding="utf-8")
        return cache

    @staticmethod
    def require_ok(result):
        if result.get("status") != "OK":
            raise RuntimeError("setup failed: " + json.dumps(result))

    def topology_file(self, graph, cache):
        path = cache.with_suffix(".topology")
        marker = path.with_name(path.name + ".json")
        identity = file_hash(cache.with_suffix(".json"))
        if not path.exists() or not marker.exists() or load_json(marker)["identity"] != identity:
            result = run_process([self.worker] + options({"action": "build-topology-index", "graph-cache": cache,
                                                          "topology-index-file": path}), 21600)
            self.require_ok(result)
            marker.write_text(json.dumps(dict(identity=identity, measurement=result)), encoding="utf-8")
        return path

    def online_index(self, query, cache):
        path = self.artifact_root / (query["graph"] + ".pool3")
        meta = path.with_suffix(".json")
        identity = dict(graph=file_hash(cache.with_suffix(".json")), delta_min=query["delta_min"], delta_max=query["delta_max"])
        if not path.exists() or not meta.exists() or load_json(meta)["identity"] != identity:
            path.parent.mkdir(parents=True, exist_ok=True)
            result = run_process([self.worker] + options({"action": "build-index", "graph-cache": cache,
                "index-file": path, "delta-min": query["delta_min"], "delta-max": query["delta_max"]}), 21600)
            self.require_ok(result)
            meta.write_text(json.dumps(dict(identity=identity, measurement=result), indent=2), encoding="utf-8")
        return path

    def layout_file(self, query, cache, rebuild=False):
        path = self.artifact_root / query["graph"] / query["gap_group"] / (query["index_type"] + ".bin")
        meta = path.with_suffix(".json")
        values = query_options(query)
        identity = dict(graph=file_hash(cache.with_suffix(".json")), delta_min=str(values["delta-min"]),
                        delta_max=str(values["delta-max"]), index_type=query["index_type"])
        if rebuild or not path.exists() or not meta.exists() or load_json(meta)["identity"] != identity:
            path.parent.mkdir(parents=True, exist_ok=True)
            with QueryProcess(self.worker, cache, self.error_file) as process:
                result = process.request("build-index", {"index-type": query["index_type"], "index-file": path,
                    "delta-min": values["delta-min"], "delta-max": values["delta-max"], "soft-timeout": 21600,
                    "max-index-bytes": 0}, 21630)
            if result["status"] == "OK":
                meta.write_text(json.dumps(dict(identity=identity, measurement=result)), encoding="utf-8")
            elif not rebuild:
                self.require_ok(result)
            return path, result
        return path, load_json(meta)["measurement"]

    def release_layout(self, query):
        path = self.artifact_root / query["graph"] / query["gap_group"] / (query["index_type"] + ".bin")
        if self.artifact_root.resolve() not in path.resolve().parents:
            raise ValueError("index artifact is outside the run directory")
        for artifact in (path, path.with_suffix(".json")):
            if artifact.exists():
                artifact.unlink()

    def execute(self, query):
        cache = self.graph_cache(query["graph"])
        if self.experiment in ("main", "response", "index_online"):
            return self.run_online(query, cache)
        if self.experiment in ("parallel", "ablation"):
            return self.run_search(query, cache)
        if self.experiment == "index_build":
            return self.layout_file(query, cache, rebuild=True)[1]
        values = query_options(query)
        values["soft-timeout"] = self.protocol["soft_seconds"]
        layout = None
        if self.experiment == "index_filter":
            layout, unused = self.layout_file(query, cache)
        with QueryProcess(self.worker, cache, self.error_file) as process:
            if layout:
                self.require_ok(process.request("load-index", {"index-type": query["index_type"], "index-file": layout}, 1200))
                values["index-type"] = query["index_type"]
            action = {"gap": "sweep", "index_filter": "index-query", "online_filter": "online-filter", "correctness": "correct"}[self.experiment]
            if self.experiment == "correctness":
                directory = self.run_root / "sequences" / query["graph"] / query["query_id"]
                values.update({"sequence-dir": directory, "max-sequences": self.protocol["max_sequences"]})
            result = process.request(action, values, self.protocol["hard_seconds"])
        return result

    def run_online(self, query, cache):
        values = dict(action="run", **query_options(query))
        values.update({"graph-cache": cache, "algorithm": query["method"], "soft-timeout": self.protocol["soft_seconds"],
                       "workers": 1, "prefix-budget-nodes": self.protocol["prefix_budget_nodes"],
                       "cpe-partial-path-cap": self.protocol["cpe_partial_path_cap"]})
        if self.experiment == "index_online":
            values.update({"index-file": self.online_index(query, cache), "topology-index-file": self.topology_file(query["graph"], cache),
                           "generation-mode": query["anchor_type"]})
        else:
            values.update({"max-style-hop": self.protocol["max_style_hop"], "generation-mode": query["generation_mode"],
                           "placement-id": query["gap_placement_id"], "gap-ratio": query["gap_ratio"]})
            if query["method"] in ("PathEnum-DFS-style", "CPE-startup-style"):
                values["topology-index-file"] = self.topology_file(query["graph"], cache)
            if query["method"] != "CPE-startup-style":
                values["response-target"] = 1000
            if self.experiment == "response":
                values["response-only"] = 1
        return run_process([self.worker] + options(values), self.protocol["hard_seconds"])

    def run_search(self, query, cache):
        values = query_options(query)
        gap = query.get("gap", "")
        if not gap:
            span = int(query["x2"]) - int(query["x1"])
            if span > 0:
                gap = "{:.4g}-{:.4g}".format(100 * int(query["delta_min"]) / span,
                                             100 * int(query["delta_max"]) / span)
        values.update({"graph-cache": cache, "graph-name": query["graph"], "dataset": query["dataset"],
                       "gap": gap, "soft-timeout": self.protocol["soft_seconds"],
                       "prefix-budget": self.protocol["prefix_budget"], "workers": query["workers"],
                       "variant": query["variant"], "window-mode": query["window_mode"], "window-seed": query["window_seed"]})
        result = run_process([self.worker] + options(values), self.protocol["hard_seconds"])
        if result["status"] == "TIMEOUT_HARD" and self.experiment == "parallel":
            result.update(status="INCOMPLETE", error="24-hour safety guard; not a 120-second censored query")
        if result["status"] == "PROCESS_ERROR" and result["_return_code"] == 0:
            lines = result["_stdout"].strip().splitlines()
            if lines:
                import csv
                fields = load_json(ROOT / "configs/schemas.json")["parallel"]["fields"]
                cells = next(csv.reader([lines[-1]], delimiter="\t"))
                if len(cells) == len(fields):
                    result.update(dict(zip(fields, cells)))
        return result

    def check_answer(self, query, result):
        if result.get("status") in ("OK", "EMPTY", "PASS") and query.get("reference_answers"):
            actual = result.get("path_count", result.get("answers", result.get("answers_dfs")))
            if actual is not None and integer(actual) != integer(query["reference_answers"]):
                result.update(status="ANSWER_MISMATCH", error="complete answer count differs from fixed reference")
        if result.get("status") == "OK" and query.get("reference_events") is not None:
            if result.get("path_graph_events", result.get("e_r")) != query["reference_events"]:
                result.update(status="SUPPORT_MISMATCH", error="support size differs from fixed reference")
        if result.get("status") == "OK" and query.get("reference_checksum"):
            if result.get("support_checksum") != query["reference_checksum"]:
                result.update(status="SUPPORT_MISMATCH", error="support checksum differs from fixed reference")

    def project(self, query, result):
        row = dict(result)
        row.update({key: value for key, value in query.items() if not key.startswith("reference_")})
        if self.experiment == "main":
            row["experiment"] = "main"
        elif self.experiment == "index_online":
            row.update(experiment="index_online", dataset=query["graph"], task_key="|".join([query["graph"], query["query_id"], query["method"]]))
        elif self.experiment == "response":
            row.update(response_run_status=result["status"], response_1000_ms_raw=result.get("response_1000_ms", ""),
                       response_run_online_ms_raw=result.get("online_total_ms", ""), measurement_mode="response_only")
        elif self.experiment == "online_filter":
            row["measurement_scope"] = "online_filter_only"
        elif self.experiment == "correctness":
            row.update(d_min=query["delta_min"], d_max=query["delta_max"])
        return row
