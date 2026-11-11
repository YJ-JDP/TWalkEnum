# Data-to-paper map

Every figure and table of the paper can be regenerated from `data/raw/` and
`queries/` alone, without downloading any graph:

```bash
python3 run.py summarize                    # -> results/paper_tables/*.tsv
python3 -m pip install -r requirements-plot.txt
python3 scripts/plot_results.py             # -> results/figures/*.pdf
```

| Paper item | Raw input | Intermediate table |
|---|---|---|
| Table I (datasets) | `data/graph_manifest.tsv` | `datasets.tsv` |
| Fig. 9 (gap selection) | `data/raw/gap.tsv` | `gap_timeouts.tsv`, `hop_bounds.tsv` |
| Fig. 10 (query and response time) | `data/raw/main.tsv`, `data/raw/response.tsv` | `query_time.tsv`, `response_time.tsv`, `main_speedups.tsv` |
| Table II (time distribution) | `data/raw/main.tsv` | `query_time_distribution.tsv` |
| Fig. 11 (time versus result count) | `data/raw/main.tsv` | `result_size.tsv` |
| Fig. 12 (peak memory) | `data/raw/main.tsv` | `memory_usage.tsv` |
| Table III (component variants) | `data/raw/ablation.tsv` | `ablation_summary.tsv`, `ablation_component_ratios.tsv` |
| Fig. 13 (parallel speedup) | `data/raw/parallel.tsv` | `parallel_scaling.tsv` |
| Table IV (index space, filter, build) | `data/raw/index_build.tsv`, `data/raw/index_filter.tsv`, `data/raw/online_filter.tsv` | `index_by_setting.tsv`, `index_comparison.tsv` |
| Fig. 14 (with and without index) | `data/raw/index_online.tsv`, `data/raw/index_online_build.tsv` | `indexed_query_time.tsv` |
| Correctness statement | `data/raw/correctness.tsv` | `correctness_summary.tsv` |

`data/raw/` holds one row per query or per configuration as written by the
runner. Nothing in this repository is a hand-edited or merged copy of a
measurement. `data/graph_manifest.tsv` records the SHA-256 of each expected
graph input, so a downloaded copy can be checked before it is used.
