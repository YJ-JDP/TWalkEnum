#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
python3 scripts/check_data.py
python3 run.py summarize
python3 scripts/plot_results.py
