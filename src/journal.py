import csv
import json
import os
import sqlite3
import time
from pathlib import Path
from .tables import write_table

class FileLock:
    def __init__(self, path):
        self.stream = Path(path).open("a+b")
        try:
            if os.name == "posix":
                import fcntl
                fcntl.flock(self.stream.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
            else:
                import msvcrt
                self.stream.write(b"0")
                self.stream.flush()
                self.stream.seek(0)
                msvcrt.locking(self.stream.fileno(), msvcrt.LK_NBLCK, 1)
        except BaseException:
            self.stream.close()
            raise

    def close(self):
        self.stream.close()

    def __enter__(self):
        return self

    def __exit__(self, *unused):
        self.close()

class ResultJournal:
    def __init__(self, directory, manifest, fields, retry_interrupted=False):
        self.directory = Path(directory)
        self.directory.mkdir(parents=True, exist_ok=True)
        self.lock = FileLock(self.directory / ".lock")
        self.fields = fields
        manifest_path = self.directory / "manifest.json"
        if manifest_path.exists():
            if json.loads(manifest_path.read_text(encoding="utf-8")) != manifest:
                self.lock.close()
                raise RuntimeError("inputs or protocol changed; choose a new output directory")
        else:
            if (self.directory / "journal.sqlite3").exists():
                raise RuntimeError("existing journal has no matching manifest")
            manifest_path.write_text(json.dumps(manifest, indent=2), encoding="utf-8")
        self.database = sqlite3.connect(str(self.directory / "journal.sqlite3"))
        self.database.execute("PRAGMA synchronous=FULL")
        self.database.execute("CREATE TABLE IF NOT EXISTS results (key TEXT PRIMARY KEY, record TEXT, row TEXT)")
        self.database.commit()
        self.pending = self.directory / "pending.json"
        if self.pending.exists():
            pending = json.loads(self.pending.read_text(encoding="utf-8"))
            if self.contains(pending["key"]):
                self.pending.unlink()
            elif retry_interrupted:
                self.pending.replace(self.directory / ("interrupted_%d.json" % time.time_ns()))
            else:
                self.close()
                raise RuntimeError("interrupted case; inspect pending.json, then use --retry-interrupted")
        self.export()

    def contains(self, key):
        return self.database.execute("SELECT 1 FROM results WHERE key=?", (key,)).fetchone() is not None

    def record(self, key):
        row = self.database.execute("SELECT record FROM results WHERE key=?", (key,)).fetchone()
        return None if row is None else json.loads(row[0])

    def begin(self, key, command):
        if self.pending.exists():
            raise RuntimeError("unresolved pending case")
        self.pending.write_text(json.dumps(dict(key=key, command=command), indent=2), encoding="utf-8")

    def commit(self, key, record, row):
        with self.database:
            self.database.execute("INSERT INTO results VALUES (?,?,?)", (key, json.dumps(record), json.dumps(row)))

        path = self.directory / "raw.tsv"
        with path.open("a", encoding="utf-8", newline="") as stream:
            writer = csv.DictWriter(stream, self.fields, delimiter="\t", extrasaction="ignore", lineterminator="\n")
            writer.writerow(row)
            stream.flush()
            os.fsync(stream.fileno())
        self.pending.unlink()

    def export(self):
        rows = [json.loads(row) for (row,) in self.database.execute("SELECT row FROM results ORDER BY rowid")]
        write_table(self.directory / "raw.tsv", rows, self.fields)

    def close(self):
        self.database.close()
        self.lock.close()

    def __enter__(self):
        return self

    def __exit__(self, *unused):
        self.close()
