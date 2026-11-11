#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
python3 -u scripts/replay_measurements.py correctness "$@"
for experiment in main response index_online parallel ablation; do
    python3 -u run.py run "$experiment" "$@"
done
for mode in gap grid online; do
    python3 -u scripts/replay_measurements.py "$mode" "$@"
done
