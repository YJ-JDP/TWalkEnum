import math
import statistics
from collections import defaultdict
from pathlib import Path
from .tables import ROOT, read_table, write_table, integer, completed

def group_rows(rows, fields):
    groups = defaultdict(list)
    for row in rows:
        groups[tuple(row.get(field, "") for field in fields)].append(row)
    return groups

def number(value):
    try:
        value = float(value)
        return value if math.isfinite(value) else None
    except (TypeError, ValueError):
        return None

def average(values):
    values = [value for value in values if value is not None]
    return statistics.mean(values) if values else ""

def capped_time(row, field="online_total_ms"):
    if "TIMEOUT" in row.get("status", ""):
        return 120000.0
    return number(row.get(field))

def response_statistics(measurements):
    rows = []
    for m in measurements:
        full = 120000.0 if "TIMEOUT" in m["full_status"] else number(m["full_query_ms_raw"])
        answers = integer(m["full_answers"])
        threshold = number(m["response_1000_ms_raw"])
        if m["full_status"] == "OK" and answers is not None and answers < 1000:
            candidate, rule, censored = full, "FEWER_THAN_1000_USE_TOTAL", 0
        elif threshold is not None:
            candidate, rule, censored = threshold, "THRESHOLD_REACHED", 0
        elif "TIMEOUT" in m["response_run_status"]:
            candidate, rule, censored = 120000.0, "TIMEOUT_BEFORE_TARGET", 1
        else:
            raise ValueError("missing response measurement: " + str(m))
        if full is None or candidate is None:
            raise ValueError("missing measured full time")
        rows.append(dict(graph=m["graph"], gap=m["gap"], query_id=m["query_id"], method=m["method"],
                         response_ms=min(candidate, full), candidate_ms=candidate, full_budgeted_ms=full,
                         rule=rule, censored=censored, upper_bound_applied=int(candidate > full)))
    return rows

def summarize(raw_directory, output_directory):
    from .paper_statistics import recompute
    return recompute(raw_directory, output_directory)
