# Graph datasets

Download graph datasets separately from the source pages below. The package includes experiment measurements and queries, but no graph datasets.

| Graph | Expected input filename | Source page |
|---|---|---|
| CM | `CollegeMsg.txt` | [Dataset source](https://snap.stanford.edu/data/CollegeMsg.html) |
| SD | `Slashdot.txt` | [Dataset source](https://konect.cc/) |
| EU | `email-Eu.txt` | [Dataset source](https://snap.stanford.edu/data/email-Eu-core-temporal.html) |
| B09 | `orbitaal-2016_07_09.txt` | [Dataset source](https://zenodo.org/records/12581515) |
| sMO | `sx-mathoverflow.txt` | [Dataset source](https://snap.stanford.edu/data/sx-mathoverflow.html) |
| SMS | `SMS-A.txt` | [Dataset source](https://snap.stanford.edu/temporal-motifs/data.html) |
| B08 | `orbitaal-2016_07_08.txt` | [Dataset source](https://zenodo.org/records/12581515) |
| FB | `Facebook.txt` | [Dataset source](https://socialnetworks.mpi-sws.org/data-wosn2009.html) |
| sAU | `sx-askubuntu.txt` | [Dataset source](https://snap.stanford.edu/data/sx-askubuntu.html) |
| PR | `Prosper.txt` | [Dataset source](https://konect.cc/) |
| WT | `wiki-talk-temporal.txt` | [Dataset source](https://snap.stanford.edu/data/wiki-talk-temporal.html) |
| sSO | `sx-stackoverflow.txt` | [Dataset source](https://snap.stanford.edu/data/sx-stackoverflow.html) |

For SMS-A, the SNAP page links to the original study and data. SD and PR use the KONECT collection; availability follows the source site. ORBITAAL provides the Bitcoin data; B08 and B09 identify the UTC dates 2016-07-08 and 2016-07-09.

## Input format

Each prepared input is a whitespace-separated text file with three integer columns: `source destination timestamp`. Keep duplicate events and their row order. Vertex IDs are assigned internally in first-appearance order. The supplied queries therefore require the corresponding prepared input, including its timestamp normalization and row order. Upstream downloads may have different headers, columns, or time units; source-page links do not assert byte-identical files.

`data/graph_manifest.tsv` records the required SHA-256 hashes, graph statistics and fingerprints. Its archive paths describe optional external prepared archives; they are not included in this package.

## Prepare local files

```bash
python3 scripts/prepare_data.py --input-dir /path/to/prepared_text_files --out /path/to/graphs --configure
```

If the prepared text files are already in the target folder:

```bash
python3 scripts/prepare_data.py --out /path/to/graphs --configure
```

Alternatively, use external prepared gzip archives (archive basenames are listed in the manifest):

```bash
python3 scripts/prepare_data.py --archive-dir /path/to/prepared_archives --out /path/to/graphs --configure
```

Check graphs after preparation:

```bash
python3 scripts/check_data.py --graphs --graph-dir /path/to/graphs
```

Inputs with mismatching hashes are rejected before the supplied workloads are executed.
