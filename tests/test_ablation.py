import subprocess
import sys
import tempfile
import unittest
import json
from decimal import Decimal
from pathlib import Path
from unittest.mock import patch

from src.ablation import FIELDS, METHODS, make_cases, run_one, worker_path
from src.tables import ROOT, read_table, write_table

class AblationTests(unittest.TestCase):
    def test_packaged_metrics_match_output_contract(self):
        schema = json.loads((ROOT/'configs/schemas.json').read_text())['ablation']
        self.assertEqual(FIELDS, schema['fields'])
        self.assertNotIn('reference_answers', FIELDS)
        self.assertNotIn('support_build_ms', FIELDS)
        rows = read_table(ROOT/schema['file'])
        self.assertEqual(len(rows), schema['rows'])
        for row in rows:
            self.assertEqual(list(row), FIELDS)
            for left, right in [('answers', 'path_count'), ('event_id_volume', 'output_edge_ids')]:
                self.assertEqual(row[left], row[right])
            if row['status'] == 'OK':
                for field in ['ranges_ms', 'plan_ms', 'search_ms',
                              'prefix_ms', 'suffix_ms', 'answers', 'event_id_volume']:
                    self.assertNotEqual(row[field], '', (row['method'], field))
            expected = Decimal(120000) if row['status'] == 'TIMEOUT' else Decimal(row['online_total_ms'])
            self.assertEqual(Decimal(row['capped_ms']), expected)

    def test_all_configurations_and_queries(self):
        cases = make_cases(read_table(ROOT/'queries/ablation.tsv'))
        self.assertEqual(len(cases), 880)
        self.assertEqual({int(r['method_id']) for r in cases}, set(METHODS))
        self.assertTrue(all('answers' not in r and r['reference_answers'] for r in cases))

    def test_full_sequence_multisets_against_independent_dfs(self):
        result = subprocess.run([str(worker_path()), '--self-test'], capture_output=True, text=True, check=True)
        self.assertIn('PASS 1760 complete event-ID multiset comparisons', result.stdout)

    def test_single_entry_runs_every_configuration_and_resumes(self):
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            graph = directory/'tiny.txt'
            graph.write_text('0 1 1\n0 1 1\n1 2 3\n1 2 4\n0 3 2\n3 2 4\n1 3 3\n0 2 8\n')
            registry = directory/'graphs.tsv'
            write_table(registry,[dict(graph='EU', graph_file=str(graph), vertices='4', events='8')])
            queries = directory/'queries.tsv'
            write_table(queries,[dict(graph='EU', dataset='tiny', gap='10-20', query_id='1',
                src_internal='0',dst_internal='2',x1='1',x2='8',delta_min='1',delta_max='3',answers='8')])
            output = directory/'runs'
            command = [sys.executable,str(ROOT/'run.py'),'run','ablation','--graphs',str(registry),
                '--queries',str(queries),'--out',str(output)]
            first = subprocess.run(command,capture_output=True,text=True,timeout=60)
            self.assertEqual(first.returncode,0,first.stdout+first.stderr)
            raw = output/'ablation/raw.tsv'
            rows = read_table(raw)
            self.assertEqual(len(rows),11)
            self.assertEqual({r['answers'] for r in rows},{'8'})
            self.assertEqual({r['status'] for r in rows},{'OK'})
            self.assertEqual(list(rows[0]), FIELDS)
            for row in rows:
                for field in ['filter_ms', 'ranges_ms', 'plan_ms',
                              'search_ms', 'prefix_ms', 'suffix_ms']:
                    self.assertNotEqual(row[field], '', (row['method'], field))
            saved = raw.read_bytes()
            second = subprocess.run(command,capture_output=True,text=True,timeout=60)
            self.assertEqual(second.returncode,0,second.stdout+second.stderr)
            self.assertEqual(raw.read_bytes(),saved)
            self.assertFalse((output/'ablation/pending.json').exists())

    def test_watchdog_does_not_copy_reference_counts(self):
        with tempfile.TemporaryDirectory() as directory:
            directory=Path(directory)
            stub=directory/'worker.py'
            stub.write_text('import time\nprint("READY\\t10",flush=True)\ntime.sleep(60)\n')
            original=subprocess.Popen
            processes=[]
            def launch(command,**kwargs):
                process=original([sys.executable,str(stub)],**kwargs)
                processes.append(process)
                return process
            q=make_cases([dict(graph='EU',query_id='1',src_internal='0',dst_internal='1',
                x1='0',x2='10',delta_min='1',delta_max='2',answers='1234')])[0]
            with patch('src.ablation.subprocess.Popen',side_effect=launch), \
                 patch('src.ablation.time.monotonic',side_effect=[0,121]):
                row=run_one(Path('worker'),Path('cache'),q,directory)
            self.assertEqual(row['status'],'TIMEOUT')
            self.assertNotIn('reference_answers', row)
            self.assertEqual(q['reference_answers'], '1234')
            self.assertEqual(row['answers'],'')
            self.assertEqual(row['path_count'],'')
            self.assertTrue(all(p.poll() is not None for p in processes))

    def test_reference_mismatch_preserves_measured_count(self):
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            stub = directory/'worker.py'
            stub.write_text(
                'import sys\nprint("READY\\t0", flush=True)\nsys.stdin.readline()\n'
                'print("TPATH_RESULT_V1\\nstatus\\tOK\\nonline_total_ms\\t1.0'
                '\\nanswers\\t2\\nevent_id_volume\\t2\\nDONE", flush=True)\n')
            original = subprocess.Popen
            def launch(command, **kwargs):
                return original([sys.executable, str(stub)], **kwargs)
            query = make_cases([dict(graph='EU', query_id='1', src_internal='0',
                dst_internal='1', x1='0', x2='10', delta_min='1', delta_max='2',
                reference_answers='3')])[0]
            with patch('src.ablation.subprocess.Popen', side_effect=launch):
                row = run_one(Path('worker'), Path('cache'), query, directory)
            self.assertEqual(row['status'], 'ERROR')
            self.assertEqual(row['termination'], 'answer_mismatch')
            self.assertEqual(row['answers'], '2')
            self.assertNotIn('reference_answers', row)

if __name__ == '__main__':
    unittest.main()
