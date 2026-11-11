import csv
import gzip
import hashlib
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from src.tables import ROOT, read_table, write_table

class ReleaseTests(unittest.TestCase):
    def test_shell_scripts_have_unix_line_endings(self):
        for path in (ROOT/"scripts").glob("*.sh"):
            self.assertNotIn(b"\r\n",path.read_bytes())

    def test_prepared_input_extracts_exact_bytes(self):
        with tempfile.TemporaryDirectory() as directory:
            source=Path(directory)/"input";source.mkdir()
            raw=b"0 1 1\n1 2 3\n"
            archive=source/"CM.txt.gz";archive.write_bytes(gzip.compress(raw,mtime=0))
            reference=dict(graph="CM",filename="CollegeMsg.txt",archive="data/graph_archives/CM.txt.gz",
                           raw_sha256=hashlib.sha256(raw).hexdigest(),archive_sha256=hashlib.sha256(archive.read_bytes()).hexdigest())
            manifest=Path(directory)/"manifest.tsv";write_table(manifest,[reference])
            destination=Path(directory)/"output"
            command=[sys.executable,str(ROOT/"scripts/prepare_data.py"),"--graph","CM","--out",str(destination),
                     "--archive-dir",str(source),"--manifest",str(manifest)]
            subprocess.run(command,check=True,capture_output=True,text=True)
            output=destination/reference["filename"]
            self.assertEqual(hashlib.sha256(output.read_bytes()).hexdigest(),reference["raw_sha256"])
            subprocess.run(command,check=True,capture_output=True,text=True)
            output.write_bytes(b"changed")
            failed=subprocess.run(command,capture_output=True,text=True)
            self.assertNotEqual(failed.returncode,0)
            self.assertEqual(output.read_bytes(),b"changed")
            text_source=source/reference["filename"];text_source.write_bytes(raw)
            text_output=Path(directory)/"text_output"
            text_command=[sys.executable,str(ROOT/"scripts/prepare_data.py"),"--graph","CM","--out",str(text_output),
                          "--input-dir",str(source),"--manifest",str(manifest)]
            subprocess.run(text_command,check=True,capture_output=True,text=True)
            self.assertEqual((text_output/reference["filename"]).read_bytes(),raw)
            missing_command=[sys.executable,str(ROOT/"scripts/prepare_data.py"),"--graph","CM","--out",str(Path(directory)/"missing_output"),
                             "--input-dir",str(Path(directory)/"missing"),"--manifest",str(manifest)]
            missing=subprocess.run(missing_command,capture_output=True,text=True)
            self.assertNotEqual(missing.returncode,0)
            self.assertIn("Graph inputs are not bundled",missing.stderr)

    def test_recorded_sessions_replay_and_resume(self):
        from src.recorded_runs import replay_measurements
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory)
            graph=root/"tiny.txt"
            graph.write_text("0 1 1\n1 2 3\n",encoding="utf-8")
            registry=root/"graphs.tsv"
            write_table(registry,[dict(graph="CM",dataset="CollegeMsg",graph_file=str(graph),vertices="3",events="2")])
            base=dict(graph="CM",dataset="CollegeMsg",query_id="1",src="0",dst="2",x1="1",x2="3",
                      delta_min="1",delta_max="2",reference_answers="1",reference_events="2",gap_group="tiny")
            builds=root/"builds.tsv"
            write_table(builds,[dict(graph="CM",gap_group="tiny",index_type=kind,d_min="1",d_max="2")
                                for kind in ["PoolIndex","L1","L2"]])
            for mode,job in [("grid","index_filter"),("online","online_filter"),("gap","gap"),("correctness","correctness")]:
                queries=root/(mode+".tsv")
                rows=[dict(base,index_type=kind) for kind in ["PoolIndex","L1","L2"]] if mode=="grid" else [base]
                write_table(queries,rows)
                out=root/mode
                replay_measurements(mode,registry,out,query_file=queries,build_file=builds)
                recorded=read_table(out/job/"raw.tsv")
                self.assertEqual(len(recorded),len(rows))
                self.assertTrue(all(r["status"] in ("OK","PASS") for r in recorded),str(recorded))
                replay_measurements(mode,registry,out,query_file=queries,build_file=builds)
                self.assertEqual(len(read_table(out/job/"raw.tsv")),len(rows))
                if mode=="grid":
                    self.assertEqual(len(read_table(out/"index_build/raw.tsv")),3)
                    self.assertFalse(list((out/"artifacts/filter").rglob("*.bin")))

    def test_temporary_filter_layout_is_released(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory)
            graph=root/"tiny.txt";graph.write_text("0 1 1\n1 2 3\n",encoding="utf-8")
            registry=root/"graphs.tsv";write_table(registry,[dict(graph="CM",dataset="CollegeMsg",graph_file=str(graph),vertices="3",events="2")])
            queries=root/"queries.tsv";write_table(queries,[dict(graph="CM",gap_group="tiny",index_type="PoolIndex",query_id="1",src="0",dst="2",x1="1",x2="3",delta_min="1",delta_max="2",reference_events="2")])
            out=root/"runs"
            command=[sys.executable,str(ROOT/"run.py"),"run","index_filter","--graphs",str(registry),"--queries",str(queries),"--out",str(out)]
            result=subprocess.run(command,capture_output=True,text=True,timeout=30)
            self.assertEqual(result.returncode,0,result.stdout+result.stderr)
            rows=read_table(out/"index_filter/raw.tsv")
            self.assertEqual(rows[0]["status"],"OK")
            self.assertEqual(rows[0]["path_graph_events"],"2")
            self.assertFalse((out/"artifacts/filter/CM/tiny/PoolIndex.bin").exists())

if __name__=="__main__":unittest.main()
