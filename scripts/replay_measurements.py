#!/usr/bin/env python3
import argparse
import sys
from pathlib import Path

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT))
from src.recorded_runs import replay_measurements

def main():
    parser=argparse.ArgumentParser(description="Replay the recorded graph-resident measurement protocols")
    parser.add_argument("mode",choices=["grid","online","gap","correctness"])
    parser.add_argument("--graphs",default=str(ROOT/"configs/graphs.tsv"))
    parser.add_argument("--out",default=str(ROOT/"runs"))
    parser.add_argument("--graph")
    parser.add_argument("--retry-interrupted",action="store_true")
    parser.add_argument("--limit",type=int)
    args=parser.parse_args()
    if args.limit is not None and args.limit < 1:parser.error("--limit must be positive")
    target=Path(args.out).resolve();data=(ROOT/"data").resolve()
    if target==data or data in target.parents:
        parser.error("new runs must not overwrite packaged data")
    replay_measurements(args.mode,args.graphs,args.out,args.graph,args.retry_interrupted,limit=args.limit)

if __name__=="__main__":main()
