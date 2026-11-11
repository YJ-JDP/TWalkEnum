import argparse
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from src.tables import read_table, write_table

def main():
    parser = argparse.ArgumentParser(description='Configure the four ablation graph inputs')
    parser.add_argument('--data-dir', required=True, type=Path)
    parser.add_argument('--out', type=Path, default=ROOT/'configs/ablation_graphs.tsv')
    args = parser.parse_args()
    rows = []
    for dataset in read_table(ROOT/'configs/datasets.tsv'):
        if dataset['name'] not in ['EU','WT','sAU','sSO']:
            continue
        path = (args.data_dir/dataset['filename']).resolve()
        if not path.is_file():
            raise FileNotFoundError(path)
        rows.append(dict(graph=dataset['name'], dataset=path.stem, graph_file=str(path),
            vertices=dataset['vertices'], events=dataset['events'], delimiter='space',
            src_column='0', dst_column='1', time_column='2', id_column='-1', skip_header='0'))
    write_table(args.out, rows)
    print('Configured four graphs:', args.out)

if __name__ == '__main__':
    main()
