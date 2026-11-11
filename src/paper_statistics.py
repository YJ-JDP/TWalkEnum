
import csv
import math
from collections import defaultdict
from decimal import Decimal, getcontext
from pathlib import Path

from .tables import ROOT, read_table, write_table

getcontext().prec = 40
D = Decimal
CAP = D(120000)
GRAPHS = ['CM', 'SD', 'EU', 'B09', 'sMO', 'SMS', 'B08', 'FB', 'sAU', 'PR', 'WT', 'sSO']
METHODS = ['TWalkEnum', 'TSPG-style', 'PathEnum-DFS-style', 'CPE-startup-style']
FOUR = ['EU', 'WT', 'sAU', 'sSO']
GAPS = ['[10%,20%]', '[20%,40%]']

def number(value):
    if value in ('', None):
        return None
    value = D(value)
    return value if value.is_finite() else None

def mean(values):
    values = [v for v in values if v is not None]
    return sum(values, D(0)) / len(values) if values else None

def groups(rows, fields):
    result = defaultdict(list)
    for row in rows:
        result[tuple(row[f] for f in fields)].append(row)
    return result

def done(row):
    return row['status'] in ('OK', 'EMPTY', 'PASS')

def elapsed(row):
    return CAP if 'TIMEOUT' in row['status'] else number(row['online_total_ms'])

def time_means(rows, fields):
    result = []
    for key, values in groups(rows, fields).items():
        times = [elapsed(r) for r in values]
        if any(t is None or t < 0 for t in times):
            raise ValueError('missing or negative time: ' + str(key))
        exact = mean(times)
        independent = math.fsum(float(t) for t in times) / len(times)
        if not math.isclose(float(exact), independent, rel_tol=1e-12, abs_tol=1e-9):
            raise ValueError('mean verification failed: ' + str(key))
        result.append(dict(zip(fields, key), n=len(values), completed=sum(done(r) for r in values),
                           timeouts=sum('TIMEOUT' in r['status'] for r in values),
                           mean_capped_ms=exact,
                           mean_completed_ms=mean([number(r['online_total_ms']) for r in values if done(r)])))
    return result

def recompute(raw_directory, output_directory):
    raw_directory, output = Path(raw_directory), Path(output_directory)
    output.mkdir(parents=True, exist_ok=True)
    raw = {p.stem: read_table(p) for p in raw_directory.glob('*.tsv')}
    emitted = []

    def save(name, rows):
        rows = list(rows)
        fields = list(dict.fromkeys(k for row in rows for k in row))
        write_table(output / (name + '.tsv'),
                    [{k: '' if row.get(k) is None else row.get(k, '') for k in fields} for row in rows], fields)
        emitted.append(dict(file=name + '.tsv', rows=len(rows)))

    manifest = read_table(ROOT / 'data/graph_manifest.tsv')
    save('datasets', [dict(graph=r['graph'], vertices=r['graph_vertex_count'],
         events=r['graph_edge_count'], span=D(r['graph_max_time'])-D(r['graph_min_time'])) for r in manifest])

    main = raw['main']
    queries = groups(main, ['graph', 'gap', 'query_id'])
    mt = time_means(main, ['graph', 'gap', 'method'])
    save('query_time', mt)
    lookup = {(r['graph'], r['gap'], r['method']): r for r in mt}
    speedups = [dict(graph=g, gap=gap, baseline=m,
                speedup=lookup[g, gap, m]['mean_capped_ms']/lookup[g, gap, METHODS[0]]['mean_capped_ms'])
                for g in GRAPHS for gap in GAPS for m in METHODS[1:]]
    save('main_speedups', speedups)

    main_by_key = {(r['graph'], r['gap'], r['query_id'], r['method']): r for r in main}
    response = []
    for r in raw['response']:
        key = tuple(r[k] for k in ['graph', 'gap', 'query_id', 'method'])
        full = main_by_key[key]
        if any(r[f] != full[f] for f in ['source_internal_id', 'target_internal_id', 'x1', 'x2', 'delta_min', 'delta_max']):
            raise ValueError('response query mismatch: ' + str(key))
        if r['full_status'] != full['status'] or number(r['full_query_ms_raw']) != number(full['online_total_ms']):
            raise ValueError('response full run mismatch: ' + str(key))
        total, threshold = elapsed(full), number(r['response_1000_ms_raw'])
        if done(full) and number(full['path_count']) < 1000:
            candidate, rule = total, 'fewer_than_1000_use_total'
        elif threshold is not None:
            candidate, rule = threshold, 'threshold_reached'
        elif 'TIMEOUT' in r['response_run_status']:
            candidate, rule = CAP, 'timeout_before_target'
        else:
            raise ValueError('missing response time: ' + str(key))
        response.append(dict(zip(['graph', 'gap', 'query_id', 'method'], key),
                        full_capped_ms=total, candidate_ms=candidate, response_ms=min(candidate, total), rule=rule,
                        censored=int(rule == 'timeout_before_target'), upper_bound_applied=int(candidate > total)))
    save('response_per_query', response)
    rt = []
    for fields in [['graph', 'gap', 'method'], ['method']]:
        for key, values in groups(response, fields).items():
            rt.append(dict(zip(fields, key), scope='/'.join(fields), n=len(values),
                      mean_response_ms=mean([r['response_ms'] for r in values]),
                      censored=sum(r['censored'] for r in values)))
    save('response_time', rt)

    distribution = []
    for (method,), values in groups(main, ['method']).items():
        row = dict(method=method, n=len(values))
        for limit in [10, 100, 1000, 10000, 60000]:
            row['lt_' + str(limit) + '_ms_pct'] = D(100)*sum(done(r) and number(r['online_total_ms']) < limit for r in values)/len(values)
        row['timeout_pct'] = D(100)*sum('TIMEOUT' in r['status'] for r in values)/len(values)
        distribution.append(row)
    save('query_time_distribution', distribution)

    throughput, common_throughput = [], []
    common_keys = {key for key, values in queries.items()
                   if len(values) == len(METHODS) and {r['method'] for r in values} == set(METHODS)
                   and all(done(r) for r in values)}
    for fields, destination, common_only in [(['graph', 'gap', 'method'], throughput, False),
                                            (['graph', 'gap', 'method'], common_throughput, True)]:
        for key, values in groups(main, fields).items():
            values = [r for r in values if done(r) and (not common_only or (r['graph'], r['gap'], r['query_id']) in common_keys)]
            answers = sum((number(r['path_count']) for r in values), D(0))
            total = sum((number(r['online_total_ms']) for r in values), D(0))
            destination.append(dict(zip(fields, key), n=len(values), answers=answers, total_ms=total,
                               answers_per_second=answers*1000/total if total else None))
    save('throughput', throughput)
    save('throughput_common', common_throughput)

    bins = [1, 10, 100, 1000, 10000, 100000, 1000000]
    classified, bucket_rows = [], []
    selected_rows = []
    for key, values in queries.items():
        exact = {number(r['path_count']) for r in values if done(r)}
        if len(exact) > 1:
            raise ValueError('completed answers disagree: ' + str(key))
        count = next(iter(exact)) if exact else None
        lower = max((number(r['path_count']) for r in values if number(r['path_count']) is not None), default=None)
        bucket = next((str(n) for n in bins if count <= n), '>1M') if count is not None else '>1M' if lower is not None and lower > 1000000 else ''
        classified.append(dict(zip(['graph', 'gap', 'query_id'], key), exact_answers=count,
                               observed_lower_bound=lower, bucket=bucket, common_completed=int(key in common_keys)))
        for row in values:
            if bucket and key in common_keys:
                time = number(row['online_total_ms'])
                if time is None or time < 0:
                    raise ValueError('invalid result-size query time: ' + str(key))
                bucket_rows.append(dict(graph=key[0], gap=key[1], method=row['method'], bucket=bucket,
                                        time_ms=time))
                selected_rows.append(dict(graph=key[0], gap=key[1], query_id=key[2],
                                          method=row['method'], bucket=bucket,
                                          answers=count, online_total_ms=time))
    save('result_size_query_buckets', classified)
    save('result_size_selected_queries', selected_rows)
    save('result_size', [dict(zip(['gap', 'method', 'bucket'], key), n_completed=len(values),
         mean_ms=mean([r['time_ms'] for r in values])) for key, values in groups(bucket_rows, ['gap', 'method', 'bucket']).items()])

    memory, memory_queries = [], []
    for graph in FOUR:
        values = [rs for (g, gap, qid), rs in queries.items() if g == graph and gap == GAPS[1]
                  and len(rs) == 4 and all(done(r) for r in rs)]
        usable = [rs for rs in values if all(number(r['query_local_peak_bytes_estimate']) is not None for r in rs)]
        for rs in usable:
            memory_queries.extend({k: row[k] for k in ['graph', 'gap', 'query_id', 'method', 'query_local_peak_bytes_estimate']} for row in rs)
        for method in METHODS:
            measured = [number(row['query_local_peak_bytes_estimate']) for rs in usable for row in rs if row['method'] == method]
            memory.append(dict(graph=graph, gap=GAPS[1], method=method, n=len(measured),
                          common_completed=len(values), missing_memory_queries=len(values)-len(usable),
                          mean_peak_bytes=mean(measured), mean_MiB=mean(measured)/1048576 if measured else None))
    save('memory_usage', memory)
    save('memory_selected_queries', memory_queries)

    aq = []
    for key, values in groups(raw['ablation'], ['graph', 'query_id']).items():
        full = [r for r in values if r['variant'] == 'full' and r['window_mode'] == 'gb']
        chrono = [r for r in values if r['variant'] == 'no_chrono']
        pivot = [r for r in values if r['variant'] == 'no_pivot']
        random = [r for r in values if r['window_mode'] == 'random']
        if list(map(len, [full, chrono, pivot, random])) != [1, 1, 1, 5]:
            raise ValueError('incomplete ablation: ' + str(key))
        aq.append(dict(graph=key[0], query_id=key[1], full_ms=elapsed(full[0]), no_chrono_ms=elapsed(chrono[0]),
                  no_pivot_ms=elapsed(pivot[0]), random_mean_ms=mean([elapsed(r) for r in random]),
                  full_timeouts=sum(not done(r) for r in full), chrono_timeouts=sum(not done(r) for r in chrono),
                  pivot_timeouts=sum(not done(r) for r in pivot), random_timeouts=sum(not done(r) for r in random)))
    save('ablation_per_query', aq)
    ab = []
    for (graph,), values in groups(aq, ['graph']).items():
        baseline = mean([r['full_ms'] for r in values])
        row = dict(graph=graph, n=len(values), mean_full_ms=baseline)
        for label, field in [('chrono', 'no_chrono_ms'), ('pivot', 'no_pivot_ms'), ('gap', 'random_mean_ms')]:
            count_field = 'random_timeouts' if label == 'gap' else label + '_timeouts'
            row[label + '_ratio'] = mean([r[field] for r in values])/baseline
            row[label + '_timeouts'] = sum(r[count_field] for r in values)
            row[label + '_lower_bound'] = int(not any(r['full_timeouts'] for r in values) and row[label + '_timeouts'] > 0)
        ab.append(row)
    save('ablation_component_ratios', ab)
    from .ablation import write_summaries
    write_summaries(raw['ablation'], output_directory)
    labels = {1:'dag_dfs_s', 2:'dag_bireach_dfs_s', 4:'chrono_direct_s',
              6:'no_chrono_s', 7:'random_s', 5:'twalk_s'}
    from .ablation import summarize_rows
    entries = summarize_rows(raw['ablation'])
    table = []
    for graph in FOUR:
        row = dict(graph=graph)
        by_method = {int(entry['method_id']):entry for entry in entries if entry['graph']==graph}
        for method,field in labels.items():
            row[field] = by_method[method]['mean_capped_s']
        table.append(row)
    save('ablation_comparison', table)
    stage = []
    full = [r for r in raw['ablation'] if r['variant'] == 'full' and r['window_mode'] == 'gb' and done(r)]
    for (graph,), values in groups(full, ['graph']).items():
        total = mean([number(r['online_total_ms']) for r in values])
        row = dict(graph=graph, n=len(values), mean_total_ms=total)
        for field in ['filter_ms', 'ranges_ms', 'plan_ms', 'prefix_ms', 'suffix_ms', 'search_ms']:
            row['mean_' + field] = mean([number(r[field]) for r in values])
            row[field + '_pct_total'] = row['mean_' + field]*100/total
        stage.append(row)
    save('stage_times', stage)

    parallel = time_means(raw['parallel'], ['graph', 'workers'])
    if not all(done(r) for r in raw['parallel']):
        raise ValueError('parallel results must be complete')
    base = {r['graph']: r['mean_capped_ms'] for r in parallel if r['workers'] == '1'}
    for row in parallel:
        row['speedup'] = base[row['graph']]/row['mean_capped_ms']
        row['efficiency'] = row['speedup']/D(row['workers'])
    save('parallel_scaling', parallel)

    gap_rows = []
    for fields in [['graph', 'bucket'], ['bucket']]:
        for key, values in groups(raw['gap'], fields).items():
            gap_rows.append(dict(zip(fields, key), scope='/'.join(fields), n=len(values),
                            timeouts=sum('TIMEOUT' in r['status'] for r in values),
                            timeout_pct=D(100)*sum('TIMEOUT' in r['status'] for r in values)/len(values)))
    save('gap_timeouts', gap_rows)

    builds = {(r['graph'], r['gap_group'], r['index_type']): r for r in raw['index_build']}
    online = groups(raw['online_filter'], ['graph', 'gap_group'])
    grid = []
    for key, values in groups(raw['index_filter'], ['graph', 'gap_group', 'index_type']).items():
        counterpart = online[key[:2]]
        identity = lambda rs: {tuple(r[f] for f in ['query_id', 'src', 'dst', 'x1', 'x2']) for r in rs}
        if identity(values) != identity(counterpart):
            raise ValueError('filter workload mismatch: ' + str(key))
        build = builds[key]
        grid.append(dict(graph=key[0], gap_group=key[1], layout=key[2], n=len(values),
                    mean_filter_ms=mean([number(r['filter_ms']) for r in values]),
                    mean_online_filter_ms=mean([number(r['filter_ms']) for r in counterpart]),
                    build_ms=number(build['build_ms']), space_bytes=number(build['space_bytes']),
                    space_MiB=number(build['space_bytes'])/1048576))
    for key, values in groups(raw['index_filter'], ['graph', 'gap_group', 'query_id']).items():
        if len(values) != 3 or len({r['support_checksum'] for r in values}) != 1:
            raise ValueError('indexed support disagreement: ' + str(key))
    save('index_by_setting', grid)
    table4 = []
    for graph in GRAPHS:
        row = dict(graph=graph)
        for layout in ['PoolIndex', 'L1', 'L2']:
            values = [r for r in grid if r['graph'] == graph and r['layout'] == layout]
            if len(values) != 10:
                raise ValueError('incomplete index grid: ' + graph)
            for field in ['mean_filter_ms', 'build_ms', 'space_MiB']:
                row[layout + '_' + field] = mean([r[field] for r in values])
            row['Online_filter_ms'] = mean([r['mean_online_filter_ms'] for r in values])
        table4.append(row)
    save('index_comparison', table4)
    index = time_means(raw['index_online'], ['dataset', 'method'])
    save('indexed_query_time', index)
    save('correctness_summary', [dict(n=len(raw['correctness']),
         passed=sum(r['status'] == 'PASS' and r['multiset_match'] == 'PASS' for r in raw['correctness']),
         all_counts_agree=all(len({r[f] for f in ['answers_twalk', 'answers_pathenum', 'answers_cpe', 'answers_tspg', 'answers_dfs']}) == 1 for r in raw['correctness']))])
    save('output_inventory', emitted.copy())
    return emitted
