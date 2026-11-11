import csv
import json
from decimal import Decimal, InvalidOperation
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

def read_table(path):
    path = Path(path)
    with path.open(encoding="utf-8-sig", newline="") as stream:
        return list(csv.DictReader(stream, delimiter="," if path.suffix == ".csv" else "\t"))

def write_table(path, rows, fields=None):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    if fields is None:
        fields = list(rows[0]) if rows else []
    temporary = path.with_suffix(path.suffix + ".tmp")
    with temporary.open("w", encoding="utf-8", newline="") as stream:
        writer = csv.DictWriter(stream, fields, delimiter="\t", extrasaction="ignore", lineterminator="\n")
        writer.writeheader()
        writer.writerows(rows)
    temporary.replace(path)

def load_json(path):
    return json.loads(Path(path).read_text(encoding="utf-8"))

def integer(value):
    try:
        number = Decimal(str(value))
        return int(number) if number.is_finite() and number == number.to_integral_value() else None
    except (InvalidOperation, TypeError, ValueError):
        return None

def completed(row):
    return row.get("status") in ("OK", "EMPTY", "PASS")
