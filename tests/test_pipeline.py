import json
import subprocess
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from src.tables import ROOT, read_table, write_table
from src.plans import make_plan, case_key, MAIN_METHODS, INDEX_METHODS
from src.processes import parse_result, run_process
from src.journal import ResultJournal
from src.benchmarks import Benchmark
from src.summary import response_statistics, summarize
from src.validate import validate_data

class ContractTests(unittest.TestCase):
    def test_no_configured_storage_or_path_caps(self):
        protocols=json.loads((ROOT/"configs/protocols.json").read_text(encoding="utf-8"))
        caps={"prefix_budget_nodes","prefix_budget","cpe_partial_path_cap",
              "max_index_bytes","max_sequences","max_style_hop"}
        for experiment, protocol in protocols.items():
            for key in caps & protocol.keys():
                self.assertEqual(protocol[key],0,(experiment,key))

    def test_all_packaged_data(self):
        self.assertEqual(validate_data()["status"], "PASS")

    def test_plans_match_data_and_order(self):
        self.assertEqual(len(make_plan("main")),48000)
        self.assertEqual(len(make_plan("response")),6798)
        self.assertEqual([r["method"] for r in make_plan("main")[:4]],
                         ["PathEnum-DFS-style","CPE-startup-style","TSPG-style","TWalkEnum"])
        self.assertEqual([r["method"] for r in make_plan("response")[:3]],
                         ["PathEnum-DFS-style","TWalkEnum","TSPG-style"])
        plan=make_plan("index_online")
        self.assertEqual(len(plan),30000)
        self.assertEqual([r["method"] for r in plan[:5]],INDEX_METHODS)
        self.assertEqual({r["workers"] for r in make_plan("parallel")},{"1","2","4","8","16","32"})
        self.assertEqual(len(make_plan("ablation")),880)

    def test_journal_recovers_projection_without_repeating_case(self):
        with tempfile.TemporaryDirectory() as directory:
            with ResultJournal(directory,{"test":1},["status","value"]) as journal:
                journal.begin("q1",["test"])
                journal.commit("q1",{"pid":123},{"status":"OK","value":"9007199254740993"})
            (Path(directory)/"raw.tsv").write_text("broken",encoding="utf-8")
            with ResultJournal(directory,{"test":1},["status","value"]) as journal:
                self.assertTrue(journal.contains("q1"))
                self.assertEqual(read_table(Path(directory)/"raw.tsv")[0]["value"],"9007199254740993")
            with self.assertRaises(RuntimeError):ResultJournal(directory,{"test":2},["status","value"])

    def test_interrupted_case_is_not_silently_repeated(self):
        with tempfile.TemporaryDirectory() as directory:
            with ResultJournal(directory,{},["status"]) as journal:journal.begin("pending",["test"])
            with self.assertRaises(RuntimeError):ResultJournal(directory,{},["status"])
            with ResultJournal(directory,{},["status"],retry_interrupted=True) as journal:
                self.assertFalse(journal.contains("pending"))
                self.assertFalse(journal.pending.exists())

    def test_complete_export_reassembles_the_same_responses(self):
        import shutil
        from src.export import export_runs
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory);runs=root/"runs"
            for name in ["main","index_online","parallel","ablation","gap","index_build","index_filter","online_filter","correctness"]:
                destination=runs/name/"raw.tsv";destination.parent.mkdir(parents=True,exist_ok=True)
                shutil.copy2(ROOT/"data/raw"/(name+".tsv"),destination)
            responses=read_table(ROOT/"data/raw/response.tsv")
            write_table(runs/"response/raw.tsv",[r for r in responses if r["measurement_mode"]=="response_only"])
            for row in read_table(ROOT/"data/raw/index_online_build.tsv"):
                saved=dict(identity=dict(delta_min=row["delta_min"],delta_max=row["delta_max"]),measurement=row)
                artifact=runs/"artifacts/index"/(row["dataset"]+".json");artifact.parent.mkdir(parents=True,exist_ok=True)
                artifact.write_text(json.dumps(saved),encoding="utf-8")
                topology=runs/"cache/index"/(row["dataset"]+".topology.json");topology.parent.mkdir(parents=True,exist_ok=True)
                topology.write_text(json.dumps(dict(measurement=row)),encoding="utf-8")
            output=root/"export"
            export_runs(runs,output)
            expected=response_statistics(responses)
            actual=response_statistics(read_table(output/"response.tsv"))
            self.assertEqual(actual,expected)
            self.assertEqual(len(read_table(output/"index_online.tsv")),30000)
            self.assertEqual(len(read_table(output/"index_online_build.tsv")),12)

    def test_milestone_does_not_become_full_query_time(self):
        record=parse_result("TPATH_RESPONSE_V1\t1000\t4.2\nTPATH_RESULT_V1\nstatus\tTIMEOUT_HARD\nonline_total_ms\t\n")
        self.assertEqual(record["response_1000_ms"],"4.2")
        self.assertEqual(record["online_total_ms"],"")

    def test_response_rules_match_saved_500_collection(self):
        rows=read_table(ROOT/"data/raw/response.tsv")
        result=response_statistics(rows)
        self.assertEqual(len(result),36000)
        self.assertEqual(sum(row["censored"] for row in result),915)
        self.assertEqual(sum(row["upper_bound_applied"] for row in result),496)

class NativeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temporary=tempfile.TemporaryDirectory()
        cls.directory=Path(cls.temporary.name)
        graph=cls.directory/"tiny.txt"
        graph.write_text("0 1 1\n0 1 1\n1 2 3\n1 2 4\n0 3 2\n3 2 4\n1 3 3\n0 2 8\n",encoding="utf-8")
        cls.registry=cls.directory/"graphs.tsv"
        write_table(cls.registry,[dict(graph="CM",dataset="CollegeMsg",graph_file=str(graph),vertices="4",events="8")])
        cls.query=dict(graph="CM",dataset="CollegeMsg",gap="10-20",query_id="1",query_seed="1",base_query_id="1",
                       generation_mode="test",gap_placement_id="0",gap_ratio="0.1",source_internal_id="0",target_internal_id="2",
                       x1="1",x2="8",delta_min="1",delta_max="3",reference_answers="8",gap_group="tiny",index_type="PoolIndex",
                       variant="full",window_mode="gb",window_seed="0",workers="1",anchor_type="test")

    @classmethod
    def tearDownClass(cls):cls.temporary.cleanup()

    def execute(self, experiment, query):
        benchmark=Benchmark(experiment,self.registry,self.directory/experiment)
        result=benchmark.execute(query)
        benchmark.check_answer(query,result)
        self.assertIn(result["status"],["OK","EMPTY","PASS","RESPONSE_TARGET_REACHED"],str(result))
        return result

    def test_main_and_index_methods_use_separate_reaped_processes(self):
        started=[]
        original=subprocess.Popen
        def launch(*args,**kwargs):
            process=original(*args,**kwargs);started.append(process);return process
        pids=[]
        with patch("src.processes.subprocess.Popen",side_effect=launch):
            for method in MAIN_METHODS:
                record=self.execute("main",dict(self.query,method=method))
                self.assertEqual(record["path_count"],"8");pids.append(record["_pid"])
                self.assertTrue(all(process.poll() is not None for process in started))
            for method in INDEX_METHODS:
                record=self.execute("index_online",dict(self.query,method=method))
                self.assertEqual(record["path_count"],"8");pids.append(record["_pid"])
                self.assertTrue(all(process.poll() is not None for process in started))
        self.assertEqual(len(set(pids)),9)

    def test_filter_layouts_and_online_support_agree(self):
        counts=[];checksums=[]
        query={k:v for k,v in self.query.items() if k!="reference_answers"}
        for layout in ["PoolIndex","L1","L2"]:
            build=self.execute("index_build",dict(query,index_type=layout,d_min="1",d_max="3"))
            self.assertGreater(int(build["space_bytes"]),0)
            filtered=self.execute("index_filter",dict(query,index_type=layout))
            counts.append(filtered["path_graph_events"]);checksums.append(filtered["support_checksum"])
            self.assertEqual(filtered["measurement_scope"],"filter_only")
            self.assertNotIn("path_count",filtered)
        online=self.execute("online_filter",query)
        counts.append(online["path_graph_events"])
        self.assertEqual(len(set(counts)),1)
        self.assertEqual(len(set(checksums)),1)

    def test_parallel_ablation_gap_and_sequence_correctness(self):
        for workers in ["1","2"]:
            self.execute("parallel",dict(self.query,workers=workers))
        for variant,window,seed in [("full","gb","0"),("no_chrono","gb","0"),("no_pivot","gb","0")]+[("full","random",str(s)) for s in range(1,6)]:
            self.execute("parallel",dict(self.query,variant=variant,window_mode=window,window_seed=seed))
        self.assertEqual(self.execute("gap",self.query)["answers"],"8")
        self.assertEqual(self.execute("correctness",self.query)["multiset_match"],"PASS")

    def test_parallel_query_without_gap_label(self):
        query = {key: value for key, value in self.query.items() if key != "gap"}
        result = self.execute("parallel", query)
        self.assertEqual(result["path_count"], "8")

    def test_cli_resume_keeps_one_record_per_configuration(self):
        queries=self.directory/"cli_queries.tsv"
        write_table(queries,[self.query])
        directory=self.directory/"cli_run"
        command=["python",str(ROOT/"run.py"),"run","main","--graphs",str(self.registry),
                 "--queries",str(queries),"--out",str(directory)]
        first=subprocess.run(command,capture_output=True,text=True,timeout=30)
        self.assertEqual(first.returncode,0,first.stdout+first.stderr)
        rows=read_table(directory/"main/raw.tsv")
        self.assertEqual(len(rows),4)
        second=subprocess.run(command,capture_output=True,text=True,timeout=30)
        self.assertEqual(second.returncode,0,second.stdout+second.stderr)
        self.assertEqual(read_table(directory/"main/raw.tsv"),rows)
        self.assertFalse((directory/"main/pending.json").exists())

    def test_response_threshold_and_below_target(self):
        graph=self.directory/"many.txt";graph.write_text("".join("0 1 %d\n"%(i%5+1) for i in range(1205)),encoding="utf-8")
        registry=self.directory/"many_graphs.tsv";write_table(registry,[dict(graph="CM",dataset="CollegeMsg",graph_file=str(graph),vertices="2",events="1205")])
        for method in MAIN_METHODS[:3]:
            query=dict(self.query,method=method,target_internal_id="1",x2="5",delta_max="2",reference_answers="1205")
            benchmark=Benchmark("response",registry,self.directory/"many")
            record=benchmark.execute(query)
            self.assertEqual(record["status"],"RESPONSE_TARGET_REACHED")
            self.assertTrue(float(record["response_1000_ms"])>=0)
        record=self.execute("response",dict(self.query,method="TWalkEnum"))
        self.assertEqual(record["status"],"OK");self.assertEqual(record["response_status"],"FEWER_THAN_1000")

if __name__=="__main__":unittest.main()
