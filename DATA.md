# Data and metrics

Graph inputs are external; sources are listed in `docs/DATASETS.md`. Prepared graphs have three columns: source, destination and integer timestamp. Preserve duplicate edges and row order. The supplied queries use the internal IDs assigned by the loader. Input hashes, ranges and fingerprints are in `data/graph_manifest.tsv`.

## Tables

| File in `data/raw/` | Records | Contents |
|---|---:|---|
| main.tsv | 48,000 | Query times, answer counts and memory measurements |
| response.tsv | 36,000 | Query-matched full and response measurements |
| gap.tsv | 9,600 | Gap-interval measurements |
| ablation.tsv | 880 | Seven configurations, with five seeds for random windows |
| parallel.tsv | 1,200 | Query times at six thread counts |
| index_build.tsv | 360 | Index space and construction times |
| index_filter.tsv | 18,000 | Three-layout filtering measurements |
| online_filter.tsv | 6,000 | Filtering without an index |
| index_online.tsv | 30,000 | Five-method query-stream measurements |
| index_online_build.tsv | 12 | Index construction measurements and setup information |
| correctness.tsv | 50 | Full edge-ID sequence multiset checks |

Schemas are in `configs/schemas.json`. Empty cells mean unavailable, not zero. Integer IDs and counts are read without conversion to floating point. Query parameters and identifiers associate records across methods.

## Fields

- `online_total_ms`: measured online execution time. Raw timeout durations can exceed the deadline; budgeted comparisons use the stated time cap.
- `path_count` / `answers`: answers produced. Counts from timed-out runs can be partial.
- `output_edge_ids` / `event_id_volume`: total edge IDs across produced answers.
- `response_1000_ms_raw`: time to the 1,000th answer when reached. The indexed experiment's `response_time_ms` instead measures the first answer.
- `measurement_mode`: whether a response threshold comes from the complete run or a separate response run. `response.tsv` pairs these measurements with the corresponding full-query record.
- `query_local_peak_bytes_estimate`: query-workspace accounting, excluding the resident graph. `peak_rss_bytes` is whole-process peak RSS and is a separate metric.
- `enumeration_ms` / `search_ms`: inclusive search time. Prefix and suffix times are components, not additional phases to add again.
- `build_ms`: construction time for the three-layout benchmark, excluding serialization.
- `index_build_ms`: construction and saving time for the query-stream index. `index_algorithm_build_ms` excludes saving.

The twelve `index_online_build.tsv` records describe the index setup for each graph. Reference answer counts are stored with the queries and are used for correctness checks; they are separate from measured answer counts.

## Ablation

The seven configurations run on the same 80 queries. Six configurations run once per query; random windows use five seeds, yielding 880 executions.

`filter_ms` measures the filtering scan and is empty when a separate scan measurement is unavailable. `ranges_ms` and `plan_ms` measure successor-range preparation and window selection. `prefix_ms` and `suffix_ms` are components of `search_ms`. DAG construction and pruning timers apply to explicit-DAG variants. An unused phase can be zero; an unavailable measurement is empty.

The count aliases `answers`/`path_count` and `event_id_volume`/`output_edge_ids` contain the same values for compatibility with the statistics interface.

## Statistics

Times are arithmetic means. Query-time comparisons assign 120,000 ms to timeouts, and speedups are ratios of means. Parallel experiments use complete query times. Calculations use unrounded values; displayed values use two decimal places.

For response time, a completed query with fewer than 1,000 answers uses its full query time. Otherwise use the recorded threshold time. A timeout before reaching the target contributes 120,000 ms. The reported response time does not exceed the corresponding budgeted full-query time.

For random windows, average the five runs within each query before computing the graph mean. Memory and answer-size comparisons use the same completed queries across methods. Memory values are means of per-query peaks.

The filtering benchmark averages within each parameter setting and then gives equal weight to the ten settings per graph. It uses a separate workload from the query-stream benchmark; its filter times are not components of the latter's online timings.

The statistics and plotting programs generate their outputs under `results/`.
