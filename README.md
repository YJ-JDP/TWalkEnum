# TWalkEnum

Code and experimental data for *TWalkEnum: Enumerating Temporal Walks with Consecutive Time-Gap Constraints*.

ChronoFilter constructs the exact edge support, GapBalance selects a temporal window, and PivotEnum shares suffix searches. PoolIndex reuses transition information across queries with the same gap constraints.

## Compile

The primary environment is Linux with GCC 9 or later, CMake 3.16 or later, and Python 3.8 or later. GCC must support C++17.

On Ubuntu:

```bash
sudo apt update
sudo apt install g++ cmake make python3 python3-pip
python3 run.py build
```

Build a single backend with:

```bash
python3 run.py build --backend ablation
```

On Windows, use MinGW-w64 GCC and Ninja. With MSYS2 UCRT64 installed at `C:\msys64`, run in PowerShell:

```powershell
$env:PATH = "C:\msys64\ucrt64\bin;" + $env:PATH
python -m pip install cmake ninja
python run.py build --generator Ninja --compiler C:/msys64/ucrt64/bin/g++.exe
```

Adjust the compiler path to your installation. The Windows procedure uses GCC, not Visual Studio's compiler.

## Test

Build the backends before running the native tests:

```bash
python3 -m unittest discover -s tests -v
python3 scripts/check_data.py
python3 scripts/verify_package.py
```

Correctness tests compare complete edge-ID sequence multisets with independent exhaustive DFS. Other tests check execution, data formats and experiment parameters.

## Minimal reproduction (no graph download)

All measurements are shipped, so the tables and figures of the paper can be
regenerated without downloading any graph:

```bash
python3 scripts/verify_package.py
python3 run.py summarize
python3 -m pip install -r requirements-plot.txt
python3 scripts/plot_results.py
```

The first command checks every shipped file against `SHA256SUMS.tsv`, the second
writes the paper tables to `results/paper_tables/`, and the last writes the
figures to `results/figures/`. The mapping between files and paper items is in
[docs/ARTIFACTS.md](docs/ARTIFACTS.md). Running the experiments themselves
additionally requires the graph inputs described below.

## Prepare graph inputs

Download graph datasets separately; sources and preparation details are in [docs/DATASETS.md](docs/DATASETS.md). Each prepared input contains three integer columns:

```text
source destination timestamp
```

Preserve duplicate edges and row order. Queries use the internal vertex IDs assigned by the supplied loader. Input hashes and graph statistics are listed in `data/graph_manifest.tsv`.

```bash
python3 scripts/prepare_data.py --input-dir /path/to/prepared_graphs --out /path/to/graphs --configure
```

## Run experiments

Query inputs are provided in `queries/`. The response, component and parallel experiments use queries from the main experiment; each experiment reads its corresponding query file.

```bash
python3 run.py run main
```

| Experiment | Measurements |
|---|---|
| `main` | Query time, answer count and query workspace |
| `response` | Time to the 1,000th answer |
| `gap` | Timeout rates under different gap intervals |
| `ablation` | Component variants and event-transition DAG baselines |
| `parallel` | Query time at different thread counts |
| `index_build` | Index construction time and space |
| `index_filter` | Filtering with three index layouts |
| `online_filter` | Filtering without an index |
| `index_online` | Query streams sharing gap constraints |
| `correctness` | Full answer multiset comparisons |

Run all ablation configurations with:

```bash
bash scripts/run_ablation.sh /path/to/graph/files
```

Ablation uses 20 queries on each of EU, WT, sAU and sSO under [10%,20%]. Six configurations run once per query; the random-window configuration runs with five seeds. This gives 11 executions per query and 880 executions in total. Each execution uses a separate process, one thread and a 120-second online limit. Graph loading is excluded. No storage or path-count budget is configured.

New measurements are written to `runs/`. To continue an interrupted run:

```bash
python3 run.py run ablation --graphs configs/ablation_graphs.tsv --retry-interrupted
```

For background execution on Linux:

```bash
nohup bash scripts/run_ablation.sh /path/to/graph/files > ablation.log 2>&1 &
```

## Data and outputs

| Location | Contents |
|---|---|
| `data/raw/` | Query measurements and index construction records |
| `queries/` | Experiment query inputs and reference counts |
| `configs/` | Experiment settings and table schemas |
| `runs/` | New execution outputs |

Performance experiments count answers without storing the complete answer set. Correctness tests construct full edge-ID sequences. Field meanings and statistical definitions are in [DATA.md](DATA.md).

Generate tables and figures locally with:

```bash
python3 run.py summarize
python3 -m pip install -r requirements-plot.txt
python3 scripts/plot_results.py
```

Generated tables and figures are written to `results/` and are not distributed with the source files. Measured times depend on the machine.

## Citation

Citation metadata are in [CITATION.cff](CITATION.cff). A version tag of this
repository accompanies the submission.

## References and permissions

Method references are in [docs/REFERENCES.md](docs/REFERENCES.md).
Code is released under the MIT license ([LICENSE](LICENSE)); measurements,
queries and documentation are released under CC BY 4.0
([LICENSE-DATA](LICENSE-DATA)). Graph inputs are not distributed here and remain
subject to their providers' terms (see [docs/DATASETS.md](docs/DATASETS.md)).
