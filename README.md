# TWalkEnum

Code and experimental data for *TWalkEnum: Enumerating Temporal Walks with Consecutive Time-Gap Constraints*.

TWalkEnum enumerates source-to-target temporal walks under lower and upper bounds on consecutive time gaps. ChronoFilter computes the exact edge support, GapBalance selects a temporal window, and PivotEnum shares suffix searches at window entries. PoolIndex reuses transition information for queries with fixed gap constraints.

## Compile

Requirements: Python 3.8+, CMake 3.16+, and a 64-bit C++17 compiler. The experiments use GCC on Linux.

```bash
python3 run.py build
```

To build only the ablation backend:

```bash
python3 run.py build --backend ablation
```

## Test

```bash
python3 -m unittest discover -s tests -v
python3 scripts/check_data.py
```

The tests compare complete edge-ID sequence multisets with independent exhaustive DFS on small graphs and check experiment execution, output formats and fixed-query plans.

## Preprocessing

Graph datasets are downloaded separately. Each prepared input has three integer columns:

```text
source destination timestamp
```

Retain duplicate edges and row order. Fixed queries use internal vertex IDs assigned by the supplied loader, so the prepared inputs must match `data/graph_manifest.tsv`. Source links and preparation instructions are in [docs/DATASETS.md](docs/DATASETS.md).

```bash
python3 scripts/prepare_data.py --input-dir /path/to/prepared_graphs --out /path/to/graphs --configure
```

Fixed workloads are in `queries/`. Newly generated queries require new measurements.

## Execute

```bash
python3 run.py run main
```

Available experiments:

| Name | Experiment |
|---|---|
| `main` | Query time, result count and query workspace |
| `response` | Time to the 1,000th result |
| `gap` | Gap interval selection |
| `ablation` | Component effects and event-transition DAG baselines |
| `parallel` | Parallel enumeration |
| `index_build` | Index construction and space |
| `index_filter` | Filtering with three index layouts |
| `online_filter` | Filtering without an index |
| `index_online` | Fixed-gap query streams |
| `correctness` | Full sequence multiset comparison |

To run all ablation configurations with one command:

```bash
bash scripts/run_ablation.sh /path/to/graph/files
```

For background execution:

```bash
nohup bash scripts/run_ablation.sh /path/to/graph/files > ablation.log 2>&1 &
```

The ablation workload has 20 queries on each of EU, WT, sAU and sSO under [10%,20%]. Seven configurations and five random windows per query give 880 executions. Each configuration runs in a separate process with one thread and a 120-second online limit. Graph loading is excluded; no storage or path-count budget is configured.

Completed configurations are retained. After an interruption, continue with:

```bash
python3 run.py run ablation --graphs configs/ablation_graphs.tsv --retry-interrupted
```

New measurements are written to `runs/`, without overwriting the supplied data. Timing results depend on the machine.

## Output

We count results without storing the complete result set in the performance experiments. Correctness tests construct complete edge-ID sequences and compare their multisets.

| Location | Contents |
|---|---|
| `data/raw/` | Per-query measurements |
| `queries/` | Fixed query workloads |
| `results/paper_tables/` | Recomputed tables and plotting inputs |
| `results/figures/` | Experimental figures |
| `runs/ablation/raw.tsv` | New ablation measurements |
| `runs/ablation/ablation_summary.tsv` | New ablation summary |

Regenerate tables and figures:

```bash
python3 run.py summarize
python3 scripts/check_paper_values.py
python3 -m pip install -r requirements-plot.txt
python3 scripts/plot_results.py
```

Table III uses six ablation configurations. The complete summary also includes EventDAG+BiReach+SuffixSharing. Random-window times are averaged within each query before computing the graph mean. Field definitions and statistical rules are in [DATA.md](DATA.md).

## Datasets

This repository includes experimental measurements and fixed queries, but no graph datasets. Download sources are listed in [docs/DATASETS.md](docs/DATASETS.md); dataset metadata are in `data/dataset_catalog.tsv` and `data/graph_manifest.tsv`.

Method references are in [docs/REFERENCES.md](docs/REFERENCES.md). Usage terms are in [RIGHTS.md](RIGHTS.md).
