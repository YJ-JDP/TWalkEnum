import hashlib
import json
import os
import queue
import signal
import subprocess
import threading
import time
from collections import defaultdict
from decimal import Decimal, ROUND_HALF_UP
from pathlib import Path

from .tables import ROOT, read_table, write_table
from .journal import FileLock, ResultJournal

GRAPHS = ['EU', 'WT', 'sAU', 'sSO']
METHODS = {
    1: 'EventDAG+DFS', 2: 'EventDAG+BiReach+DFS',
    3: 'EventDAG+BiReach+SuffixSharing', 4: 'ChronoFilter+DirectDFS',
    5: 'TWalkEnum', 6: 'TWalkEnum w/o ChronoFilter',
    7: 'TWalkEnum with random windows',
}
PAPER_METHODS = [1, 2, 4, 6, 7, 5]
VARIANTS = {1: 'dag_dfs', 2: 'dag_bireach', 3: 'dag_sharing',
            4: 'no_pivot', 5: 'full', 6: 'no_chrono', 7: 'full'}
FIELDS = [
    'graph', 'dataset', 'gap', 'query_id',
    'query_seed', 'method', 'method_id', 'variant',
    'window_mode', 'window_seed', 'workers', 'run_order',
    'src_internal', 'dst_internal', 'x1', 'x2',
    'delta_min', 'delta_max', 'status', 'termination',
    'online_total_ms', 'capped_ms', 'response_1000_ms', 'response_ms',
    'answers', 'event_id_volume', 'path_count', 'output_edge_ids',
    'dag_build_ms', 'dag_prune_ms', 'filter_ms', 'ranges_ms',
    'plan_ms', 'search_ms', 'prefix_ms', 'suffix_ms',
    'search_states', 'prefix_states', 'suffix_states', 'active_pivots',
    'stored_prefixes', 'eligible_events', 'retained_events', 'dag_arcs',
    'retained_arcs', 'exit_code',
]

ACTIVE = None


def log(message):
    print(time.strftime('[%Y-%m-%d %H:%M:%S] ') + message, flush=True)


def options(values):
    return [item for key, value in values.items() for item in ('--' + key, str(value))]


def worker_path():
    name = 'ablation_worker.exe' if os.name == 'nt' else 'ablation_worker'
    return ROOT / 'kernels/ablation/build/bin' / name


def case_key(row):
    return ':'.join(str(row[k]) for k in ['graph', 'query_id', 'method_id', 'window_seed'])


def make_cases(queries):
    cases = []
    for graph in dict.fromkeys(q['graph'] for q in queries):
        for index, q in enumerate(q for q in queries if q['graph'] == graph):
            jobs = [(m, seed) for m in METHODS for seed in (range(1, 6) if m == 7 else [0])]
            offset = index % len(jobs)
            jobs = jobs[offset:] + jobs[:offset]
            for order, (method, seed) in enumerate(jobs, 1):
                row = dict(q)
                row['reference_answers'] = q.get('reference_answers', q.get('answers', ''))
                row.pop('answers', None)
                row.update(method=METHODS[method], method_id=str(method), variant=VARIANTS[method],
                           window_mode='random' if method == 7 else 'none' if method in (1,2,4) else 'gb',
                           window_seed=str(seed), workers='1', run_order=str(order))
                cases.append(row)
    return cases


def project(row):
    row = dict(row)
    for alias, key in [('path_count', 'answers'), ('output_edge_ids', 'event_id_volume'),
                       ('stored_prefixes', 'stored_prefix_count')]:
        if row.get(key, '') != '':
            row[alias] = row[key]
    return {key: row.get(key, '') for key in FIELDS}


def check_counts(rows):
    completed = [r for r in rows if r['status'] == 'OK']
    values = {(Decimal(r['answers']), Decimal(r['event_id_volume'])) for r in completed}
    if len(values) > 1:
        raise RuntimeError('completed configurations disagree on answer count or edge-ID total')



def summarize_rows(rows):
    groups = defaultdict(list)
    for r in rows:
        groups[r['graph'], r['method_id']].append(r)
    summaries = []
    for graph in GRAPHS:
        for method in METHODS:
            part = groups.get((graph, str(method)), [])
            if not part:
                continue
            by_query = defaultdict(list)
            for r in part:
                by_query[r['query_id']].append(r)
            expected = 5 if method == 7 else 1
            usable = [rr for rr in by_query.values() if len(rr) == expected
                      and all(r['status'] in ('OK', 'TIMEOUT') for r in rr)]
            times = [sum(Decimal(r['capped_ms']) for r in rr)/expected for rr in usable]
            avg = sum(times)/len(times) if times else None
            summaries.append(dict(graph=graph, method=METHODS[method], method_id=method,
                n=len(usable), executions=len(part), completed=sum(r['status'] == 'OK' for r in part),
                timeouts=sum(r['status'] == 'TIMEOUT' for r in part),
                errors=sum(r['status'] not in ('OK', 'TIMEOUT') for r in part),
                mean_capped_ms=str(avg) if avg is not None else '',
                mean_capped_s=str(avg/1000) if avg is not None else ''))
    return summaries


def write_summaries(rows, directory):
    directory = Path(directory)
    directory.mkdir(parents=True, exist_ok=True)
    summary = summarize_rows(rows)
    write_table(directory/'ablation_summary.tsv', summary)
    wide = []
    for graph in GRAPHS:
        by_method = {int(r['method_id']): r for r in summary if r['graph'] == graph}
        if not by_method:
            continue
        row = dict(graph=graph)
        for method in PAPER_METHODS:
            value = by_method.get(method, {}).get('mean_capped_s', '')
            row[METHODS[method]] = str(Decimal(value).quantize(Decimal('.01'), rounding=ROUND_HALF_UP)) if value else ''
        wide.append(row)
    write_table(directory/'ablation_query_time_s.tsv', wide)
    tex = [r'\begin{table*}[t]', r'\centering\fontsize{8}{9.6}\selectfont',
        r'\caption{Mean query time of component variants (s)}', r'\label{tab:ablation}',
        r'\begin{tabular}{ccccccc}', r'\hline',
        r'Graph & \shortstack{EventDAG\\+DFS} & \shortstack{EventDAG\\+BiReach+DFS} & '
        r'\shortstack{ChronoFilter\\+DirectDFS} & \shortstack{w/o\\ChronoFilter} & '
        r'\shortstack{Random\\window} & TWalkEnum \\', r'\hline']
    for row in wide:
        values = [row[METHODS[m]] for m in PAPER_METHODS]
        available = [Decimal(v) for v in values if v]
        smallest = min(available) if available else None
        displayed = [r'\textbf{'+v+'}' if v and Decimal(v)==smallest else v for v in values]
        tex.append(' & '.join([row['graph']]+displayed)+r' \\')
    tex += [r'\hline',r'\end{tabular}',r'\end{table*}']
    (directory/'ablation_comparison.tex').write_text('\n'.join(tex)+'\n', encoding='utf-8')
    lines = ['Mean online query time (s); timeouts contribute 120 s.', '']
    lines += ['\t'.join(str(r.get(k, '')) for k in wide[0]) for r in wide] if wide else []
    if wide:
        lines.insert(2, '\t'.join(wide[0]))
    (directory/'ablation.txt').write_text('\n'.join(lines)+'\n', encoding='utf-8')
    return summary


def run_one(worker, cache, query, results):
    global ACTIVE
    args = {'cache': cache, 'method': query['method_id'], 'source': query['src_internal'],
            'target': query['dst_internal'], 'x1': query['x1'], 'x2': query['x2'],
            'delta-min': query['delta_min'], 'delta-max': query['delta_max'],
            'window-seed': query['window_seed']}
    row = {key: query.get(key, '') for key in FIELDS}
    for key in ['answers', 'event_id_volume', 'path_count', 'output_edge_ids']:
        row[key] = ''
    events = queue.Queue()
    with (results/'worker_errors.log').open('a', encoding='utf-8') as errors:
        process = subprocess.Popen([str(worker)] + options(args), stdin=subprocess.PIPE,
            stdout=subprocess.PIPE, stderr=errors, text=True, bufsize=1)
        ACTIVE = process
        def read():
            try:
                for line in process.stdout:
                    events.put(line.rstrip('\r\n'))
            finally:
                events.put(None)
        reader = threading.Thread(target=read, daemon=True)
        reader.start()
        end = None
        response = None
        result_started = False
        watchdog = False
        try:
            while True:
                remaining = None if end is None else end-time.monotonic()
                if remaining is not None and remaining <= 0:
                    watchdog = True
                    break
                try:
                    line = events.get(timeout=remaining)
                except queue.Empty:
                    watchdog = True
                    break
                if line is None or (line == 'DONE' and result_started):
                    break
                if line.startswith('READY\t'):
                    end = time.monotonic()+120
                    process.stdin.write('GO\n')
                    process.stdin.flush()
                elif line.startswith('STAGE\t'):
                    _, key, value = line.split('\t')
                    row[key] = value
                elif line.startswith('TPATH_RESPONSE_V1\t'):
                    response = line.split('\t')[2]
                elif line == 'TPATH_RESULT_V1':
                    result_started = True
                elif result_started and '\t' in line:
                    key, value = line.split('\t', 1)
                    row[key] = value
            if watchdog:
                process.kill()
                row.update(status='TIMEOUT', termination='120s_watchdog', online_total_ms='120000.000000')
            process.wait()
            row['exit_code'] = process.returncode
            if not watchdog:
                if process.returncode != 0 or row.get('status') not in ('OK', 'TIMEOUT'):
                    row.update(status='ERROR', termination='worker_exit_'+str(process.returncode))
                else:
                    row['termination'] = 'completed' if row['status'] == 'OK' else '120s_deadline'
            if response is not None:
                row['response_1000_ms'] = response
            if row.get('status') in ('OK', 'TIMEOUT'):
                row['capped_ms'] = '120000.000000' if row['status'] == 'TIMEOUT' else row['online_total_ms']
                reached = row.get('response_1000_ms', '')
                row['response_ms'] = reached if reached and float(reached) >= 0 else row['capped_ms']
            reference = query.get('reference_answers', '')
            if row.get('status') == 'OK' and reference and Decimal(row['answers']) != Decimal(reference):
                row.update(status='ERROR', termination='answer_mismatch')
            return project(row)
        finally:
            if process.poll() is None:
                process.kill()
                process.wait()
            process.stdin.close()
            reader.join()
            process.stdout.close()
            ACTIVE = None


def graph_cache(worker, graph, info, output):
    source = Path(info['graph_file']).expanduser().resolve()
    stat = source.stat()
    identity = dict(source=str(source), size=stat.st_size, mtime=stat.st_mtime_ns,
        loader={k: info.get(k, '') for k in ['src_column','dst_column','time_column','id_column','delimiter','skip_header']})
    path = output/'cache/ablation'/(graph+'.bin')
    marker = path.with_suffix('.json')
    path.parent.mkdir(parents=True, exist_ok=True)
    if not path.exists() or not marker.exists() or json.loads(marker.read_text(encoding='utf-8')) != identity:
        args = {'prepare': source, 'cache': str(path)+'.tmp'}
        args.update({k.replace('_','-'):v for k,v in identity['loader'].items() if v != ''})
        result = subprocess.run([str(worker)]+options(args), capture_output=True, text=True, check=True)
        fields = result.stdout.strip().split('\t')
        if len(fields) != 3 or fields[0] != 'GRAPH':
            raise RuntimeError('invalid graph preparation result')
        for measured, field in zip(fields[1:], ['vertices','events']):
            if info.get(field) and measured != info[field]:
                raise RuntimeError('graph mapping/statistics differ: '+graph+' '+field)
        os.replace(str(path)+'.tmp', path)
        marker.write_text(json.dumps(identity, indent=2), encoding='utf-8')
    return path


def run_experiment(registry, output_root, graph=None, query_file=None, limit=None,
                   retry_interrupted=False, dry_run=False, worker=None):
    from .plans import make_plan
    cases = make_plan('ablation', query_file, graph)
    if limit is not None:
        cases = cases[:limit]
    if dry_run:
        print(json.dumps(dict(experiment='ablation', cases=len(cases), methods=METHODS,
            workers=1, timeout_seconds=120, fresh_process_per_case=True, first_case=cases[0]), indent=2))
        return
    worker = Path(worker) if worker else worker_path()
    if not worker.is_file():
        raise FileNotFoundError('Build the ablation backend first: '+str(worker))
    output = Path(output_root).resolve()
    directory = output/'ablation'
    directory.mkdir(parents=True, exist_ok=True)
    graphs = {r['graph']:r for r in read_table(registry)}
    graph_identity = {}
    for name in {case['graph'] for case in cases}:
        source = Path(graphs[name]['graph_file']).resolve()
        stat = source.stat()
        graph_identity[name] = dict(path=str(source), size=stat.st_size, mtime=stat.st_mtime_ns)
    manifest = dict(worker_sha256=hashlib.sha256(worker.read_bytes()).hexdigest(),
        driver_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
        cases_sha256=hashlib.sha256(json.dumps(cases,sort_keys=True).encode()).hexdigest(),
        registry_sha256=hashlib.sha256(Path(registry).read_bytes()).hexdigest(), graphs=graph_identity)
    def stopped(signum, frame):
        raise KeyboardInterrupt
    previous = signal.signal(signal.SIGTERM, stopped)
    try:
        with FileLock(ROOT/'.benchmark.lock'):
            check = subprocess.run([str(worker),'--self-test'], capture_output=True, text=True, check=True)
            (directory/'correctness.txt').write_text(check.stdout, encoding='utf-8')
            log(check.stdout.strip())
            with ResultJournal(directory, manifest, FIELDS, retry_interrupted) as journal:
                cache = {}
                completed = read_table(directory/'raw.tsv')
                groups = defaultdict(list)
                for row in completed:
                    groups[row['graph'], row['query_id']].append(row)
                for rows in groups.values():
                    check_counts(rows)
                for case in cases:
                    key = case_key(case)
                    if journal.contains(key):
                        if journal.record(key).get('status') not in ('OK','TIMEOUT'):
                            raise RuntimeError('Failed configuration requires inspection: '+key)
                        continue
                    name = case['graph']
                    if name not in cache:
                        log('Preparing '+name+'; graph loading is excluded from online time')
                        cache[name] = graph_cache(worker,name,graphs[name],output)
                    journal.begin(key,case)
                    log('START '+key+' '+case['method'])
                    result = run_one(worker,cache[name],case,directory)
                    journal.commit(key,result,result)
                    completed.append(result)
                    if result['status'] not in ('OK','TIMEOUT'):
                        raise RuntimeError('Worker failed: '+key)
                    check_counts([r for r in completed if r['graph']==name and r['query_id']==case['query_id']])
                    write_summaries(completed,directory)
                    log('{} {} ms={}'.format(key,result['status'],result['capped_ms']))
                write_summaries(completed,directory)
            log('ABLATION FINISHED')
    finally:
        if ACTIVE is not None and ACTIVE.poll() is None:
            ACTIVE.kill()
            ACTIVE.wait()
        signal.signal(signal.SIGTERM,previous)
