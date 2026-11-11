# Data and metrics

Graph inputs are external; source links are in `docs/DATASETS.md`. They have three columns: source, destination and integer timestamp. Row order and duplicate events are retained. Fixed-query internal IDs require these exact inputs. See `data/graph_manifest.tsv` for hashes, sizes, ranges and fingerprints, and `data/dataset_catalog.tsv` for original sources.

| Raw table | Rows | Use |
|---|---:|---|
| main.tsv | 48,000 | Query times, completion, throughput, result size and memory |
| response.tsv | 36,000 | Query-matched response times |
| gap.tsv | 9,600 | Gap-selection timeout rates |
| ablation.tsv | 880 | Seven ablation configurations, including five random windows per query |
| parallel.tsv | 1,200 | Complete query times at six thread counts |
| index_build.tsv | 360 | Standalone index space and construction |
| index_filter.tsv | 18,000 | Three-layout filtering comparison |
| online_filter.tsv | 6,000 | Online filtering on matching queries |
| index_online.tsv | 30,000 | Five-method fixed-gap query stream |
| index_online_build.tsv | 12 | Setup and size metadata for that stream |
| correctness.tsv | 50 | Complete event-ID sequence multiset checks |

All 150,102 query/configuration records are retained. Fixed-index setup identifies the index used online; its times are not mixed into Table IV's separate grid workload. Schemas are in `configs/schemas.json`. Empty values mean unavailable, not zero. Integer IDs and counts are not coerced through floating point.

## Fields

- `online_total_ms`: online timing. Hard-stop records may contain driver guard durations; timeout statistics use the configured 120-second cap.
- `path_count` / `answers`: produced answers; timeout counts can be partial. Zero at timeout does not prove no answer.
- `response_1000_ms_raw`: threshold time when recorded. Indexed `response_time_ms` means first answer, not the 1,000-answer metric.
- `query_local_peak_bytes_estimate`: query workspace accounting, excluding the resident graph. `peak_rss_bytes` is whole-process peak RSS and is separate.
- `enum_ms` / `enumeration_ms`: inclusive enumeration timers. Prefix/suffix are components, not additional independent phases.
- Grid `build_ms` excludes serialization. Fixed-index `index_build_ms` includes saving; `index_algorithm_build_ms` is construction only.

## Statistics

`src/paper_statistics.py` uses Decimal arithmetic, independently checking time means with `math.fsum`. Round presentation to two decimals; compute with unrounded values.

Query times use arithmetic means with timeouts assigned 120,000 ms. Speedups are ratios of arithmetic means.

Pair responses with main records by query and method. Complete queries below 1,000 answers use total time; otherwise use the threshold. Timeout before the target contributes the limit and a censor flag. Bound response time by the corresponding capped total. Raw timers are unchanged.

For ablation, average five random windows within each query before averaging across twenty queries per graph. Table III reports arithmetic mean times for six configurations; the additional EventDAG+BiReach+SuffixSharing configuration is included in the complete summary. Each query/configuration runs in a separate process with one thread and a 120-second online limit. Graph loading is excluded. On watchdog termination, unavailable answer counts remain empty; reference_answers is a separate field.

Parallel uses complete times on the same fifty queries per thread count. A safety kill is incomplete, not a capped result.

Memory uses EU, WT, sAU and sSO under [20%,40%], with all four methods complete and all four memory measurements present. Average per-query peaks; retain common-completed counts.

Result-size curves use queries completed by all four methods in each answer-count group. Unknown counts stay unknown; partial counts assign the last bucket only when more than one million answers are certified.

Throughput divides total answers by total time of relevant completed queries. Cross-method throughput comparisons use common-completed queries.

Table IV computes means per standalone parameter setting, then equal-weight means across ten settings. Space/build records come from those same settings. Filter times are not additive components of the separate indexed end-to-end query timings.

The gap-selection figure uses measured timeout rates and theoretical hop bounds. Its first interval has no uniform finite bound and no line point.

## Ablation fields

`support_build_ms` measures candidate filtering and support-graph construction together. When those phases were timed separately, it is their arithmetic sum. `filter_ms` measures only the filtering scan and is empty when no separate scan measurement is available. `ranges_ms` and `plan_ms` measure successor-range preparation and window selection. `search_ms` includes prefix collection and suffix search; its components must not be added again. These stage timers need not sum to the complete online time.

DAG construction and pruning timers apply to explicit-DAG variants. An unused phase can be zero; an unavailable measurement is empty. The ablation table omits memory and path-length diagnostics. Memory comparisons use the separate main-experiment records. `answers`/`path_count` and `event_id_volume`/`output_edge_ids` are matching count aliases for the shared statistics interface.
