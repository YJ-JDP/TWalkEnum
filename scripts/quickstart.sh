#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
python3 scripts/verify_package.py
python3 run.py build
python3 -m unittest discover -s tests -v
python3 scripts/check_data.py
python3 run.py summarize
python3 scripts/check_paper_values.py
