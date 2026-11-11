#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
if [ "$#" -ne 1 ]; then
  echo "Usage: bash scripts/run_ablation.sh /absolute/path/to/graph/files" >&2
  exit 2
fi
python3 scripts/configure_ablation.py --data-dir "$1"
python3 run.py build --backend ablation
python3 -u run.py run ablation --graphs configs/ablation_graphs.tsv
