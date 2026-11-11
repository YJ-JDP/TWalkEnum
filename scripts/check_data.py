#!/usr/bin/env python3

import argparse
import csv
import gzip
import hashlib
import json
import sys
from collections import Counter, defaultdict
from decimal import Decimal, ROUND_CEILING
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from src.tables import read_table, write_table
from src.validate import validate_data
from src.plans import make_plan

def digest(stream):
    value = hashlib.sha256()
    for block in iter(lambda: stream.read(4*1024*1024), b''):
        value.update(block)
    return value.hexdigest()

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--graphs', action='store_true', help='also verify external graph inputs')
    parser.add_argument('--graph-dir', type=Path, help='directory containing prepared text graphs')
    parser.add_argument('--archive-dir', type=Path, help='directory containing prepared gzip archives')
    args = parser.parse_args()
    report = validate_data()
    checks = []

    def require(name, condition):
        if not condition:
            raise ValueError(name)
        checks.append(dict(check=name, status='PASS'))

    raw = {p.stem: read_table(p) for p in (ROOT/'data/raw').glob('*.tsv')}
    schema = json.loads((ROOT/'configs/schemas.json').read_text())
    require('only documented raw tables', set(raw) == set(schema))
    for name, rows in raw.items():
        with (ROOT/'data/raw'/(name+'.tsv')).open(encoding='utf-8-sig', newline='') as stream:
            reader = csv.DictReader(stream, delimiter='\t')
            require(name+' rectangular rows', all(None not in row and all(v is not None for v in row.values()) for row in reader))
        require(name+' acceptable status', all(not r.get('status') or r['status'] in
            ('OK', 'EMPTY', 'PASS', 'TIMEOUT', 'TIMEOUT_SOFT', 'TIMEOUT_HARD') for r in rows))
        if 'online_total_ms' in rows[0]:
            require(name+' completed times present', all(r['online_total_ms'] != '' and Decimal(r['online_total_ms']).is_finite()
                and Decimal(r['online_total_ms']) >= 0 for r in rows if r['status'] in ('OK', 'EMPTY')))

    ab = raw['ablation']
    key = lambda r: tuple(r[k] for k in ['graph', 'query_id', 'variant', 'window_mode', 'window_seed'])
    cases = make_plan('ablation')
    require('ablation measured and planned keys identical', {key(r) for r in ab} == {key(r) for r in cases})
    require('ablation unique configurations', len({key(r) for r in ab}) == 880)
    by_case = {key(r): r for r in cases}
    require('ablation complete counts match workload', all(Decimal(r['path_count']) == Decimal(by_case[key(r)]['reference_answers'])
        for r in ab if r['status'] == 'OK'))
    require('ablation twenty queries per graph', Counter(r['graph'] for r in ab) == {'EU':220,'WT':220,'sAU':220,'sSO':220})

    main_rows = raw['main']
    for gap, bounds in [('[10%,20%]', ('.1','.2')), ('[20%,40%]', ('.2','.4'))]:
        require('main ceil gaps '+gap, all(
            Decimal(r['delta_min']) == ((Decimal(r['x2'])-Decimal(r['x1']))*Decimal(bounds[0])).to_integral_value(rounding=ROUND_CEILING)
            and Decimal(r['delta_max']) == ((Decimal(r['x2'])-Decimal(r['x1']))*Decimal(bounds[1])).to_integral_value(rounding=ROUND_CEILING)
            for r in main_rows if r['gap'] == gap))
    full = {tuple(r[k] for k in ['graph','gap','query_id','method']):r for r in main_rows}
    require('response matches complete run', all(
        r['full_status'] == full[tuple(r[k] for k in ['graph','gap','query_id','method'])]['status']
        and r['full_query_ms_raw'] == full[tuple(r[k] for k in ['graph','gap','query_id','method'])]['online_total_ms']
        and all(r[f] == full[tuple(r[k] for k in ['graph','gap','query_id','method'])][f]
                for f in ['source_internal_id','target_internal_id','x1','x2','delta_min','delta_max'])
        for r in raw['response']))

    filters = defaultdict(list)
    for r in raw['index_filter']:
        filters[r['graph'],r['gap_group'],r['query_id']].append(r)
    require('all indexed filter supports identical', all(len(rs)==3 and len({r['support_checksum'] for r in rs})==1
        and len({r['path_graph_events'] for r in rs})==1 for rs in filters.values()))
    online = {(r['graph'],r['gap_group'],r['query_id']):r for r in raw['online_filter']}
    require('online and indexed filter supports identical', all(key in online and all(
        r['path_graph_events']==online[key]['path_graph_events'] and all(r[f]==online[key][f] for f in ['src','dst','x1','x2'])
        for r in rs) for key,rs in filters.items()))

    correct = raw['correctness']
    require('all fifty complete sequence checks passed', len(correct)==50 and all(
        r['multiset_match']=='PASS' and r['status']=='PASS' and len({r[f] for f in
        ['answers_twalk','answers_pathenum','answers_cpe','answers_tspg','answers_dfs']})==1 for r in correct))
    require('gap twelve graphs eight groups one hundred queries', len(Counter((r['graph'],r['bucket']) for r in raw['gap']))==96
        and set(Counter((r['graph'],r['bucket']) for r in raw['gap']).values())=={100})
    par = defaultdict(list)
    for r in raw['parallel']:
        par[r['graph'],r['workers']].append(r)
    require('parallel same fifty queries at every thread count', len(par)==24 and all(len(rs)==50 and
        {r['query_id'] for r in rs}=={r['query_id'] for r in par[g,'1']} for (g,p),rs in par.items()))

    graph_checks = []
    if args.graphs:
        for row in read_table(ROOT/'data/graph_manifest.tsv'):
            if args.graph_dir:
                path = args.graph_dir / row['filename']
                with path.open('rb') as stream:
                    require('graph contents hash '+row['graph'], digest(stream)==row['raw_sha256'])
            else:
                path = args.archive_dir / Path(row['archive']).name if args.archive_dir else ROOT/row['archive']
                if not path.is_file():
                    raise FileNotFoundError('Graph inputs are external; pass --graph-dir or --archive-dir. See docs/DATASETS.md')
                with path.open('rb') as stream:
                    require('archive hash '+row['graph'], digest(stream)==row['archive_sha256'])
                with gzip.open(path,'rb') as stream:
                    require('graph contents hash '+row['graph'], digest(stream)==row['raw_sha256'])
            graph_checks.append(dict(graph=row['graph'], archive_sha256=row['archive_sha256'], raw_sha256=row['raw_sha256'], status='PASS'))
        write_table(ROOT/'validation/graph_integrity.tsv',graph_checks)

    write_table(ROOT/'validation/data_checks.tsv',checks)
    report.update(deep_checks=len(checks), graphs_verified=len(graph_checks), raw_rows=sum(len(rs) for rs in raw.values()))
    (ROOT/'validation/data_audit.json').write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8')
    print(json.dumps(report,indent=2))

if __name__ == '__main__':
    main()
