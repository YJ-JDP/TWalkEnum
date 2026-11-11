#!/usr/bin/env python3

import json
import sys
from decimal import Decimal, ROUND_HALF_UP
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from src.tables import read_table, write_table

def main():
    (ROOT / "validation").mkdir(parents=True, exist_ok=True)
    directory = ROOT/'results/paper_tables'
    cache = {}
    checks = []

    def rows(name):
        if name not in cache:
            cache[name] = read_table(directory/(name+'.tsv'))
        return cache[name]

    def select(name, **key):
        found = [r for r in rows(name) if all(r.get(k)==str(v) for k,v in key.items())]
        if len(found)!=1:
            raise ValueError('ambiguous table lookup: '+name+str(key))
        return found[0]

    def check(label, value, expected, scientific=False):
        value = Decimal(value)
        display = f'{value:.2E}' if scientific else str(value.quantize(Decimal('.01'),rounding=ROUND_HALF_UP))
        passed = Decimal(display)==Decimal(expected)
        checks.append(dict(item=label,computed=value,display=display,expected=expected,status='PASS' if passed else 'FAIL'))

    for item in json.loads((ROOT/'docs/paper_values.json').read_text()):
        row = select(item['table'],**item['key'])
        check(item['table']+str(item['key'])+':'+item['field'],row[item['field']],item['expected'])

    for method, value in [('TSPG-style','59.62'),('PathEnum-DFS-style','4048.30'),('CPE-startup-style','2835.39')]:
        check('maximum speedup '+method,max(Decimal(r['speedup']) for r in rows('main_speedups') if r['baseline']==method),value)
    for method,value in [('TWalkEnum','87.24'),('TSPG-style','3115.89'),('PathEnum-DFS-style','11052.19')]:
        check('pooled response '+method,select('response_time',method=method,scope='method')['mean_response_ms'],value)
    for graph, method, value in [('sSO','PathEnum-DFS-style','6.84'),('sSO','CPE-startup-style','12.20'),('WT','TSPG-style','14.49')]:
        numerator=Decimal(select('memory_usage',graph=graph,method=method)['mean_peak_bytes'])
        denominator=Decimal(select('memory_usage',graph=graph,method='TWalkEnum')['mean_peak_bytes'])
        check('memory ratio '+graph+' '+method,numerator/denominator,value)
    stage=rows('stage_times')
    check('filter share min',min(Decimal(r['filter_ms_pct_total']) for r in stage),'0.19')
    check('filter share max',max(Decimal(r['filter_ms_pct_total']) for r in stage),'3.69')
    for graph,value in [('EU','9.79'),('WT','9.59'),('sAU','6.30'),('sSO','3.97')]:
        check('32-thread speedup '+graph,select('parallel_scaling',graph=graph,workers='32')['speedup'],value)
    efficiencies=[Decimal(r['efficiency']) for r in rows('parallel_scaling') if r['workers']=='8']
    check('8-thread efficiency min',min(efficiencies),'0.25')
    check('8-thread efficiency max',max(efficiencies),'0.62')
    ratios=[]
    for graph in ['CM','SD','EU','B09','sMO','SMS','B08','FB','sAU','PR','WT','sSO']:
        base=Decimal(select('indexed_query_time',dataset=graph,method='TWalkEnum')['mean_capped_ms'])
        indexed=Decimal(select('indexed_query_time',dataset=graph,method='PoolIndex+TWalkEnum')['mean_capped_ms'])
        ratios.append(base/indexed)
    check('index speedup min',min(ratios),'1.48')
    check('index speedup max',max(ratios),'6.52')
    check('peak throughput',max(Decimal(r['answers_per_second']) for r in rows('throughput') if r['method']=='TWalkEnum'),'5.26E8',True)
    write_table(ROOT/'validation/paper_value_checks.tsv',checks)
    failures=[r for r in checks if r['status']=='FAIL']
    (ROOT/'validation/paper_value_check.json').write_text(json.dumps(dict(checks=len(checks),failed=len(failures)),indent=2)+'\n')
    if failures:
        raise ValueError(str(failures))
    print('Verified',len(checks),'paper table and text values.')

if __name__=='__main__':
    main()
