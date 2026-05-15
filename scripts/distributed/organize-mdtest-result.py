#!/bin/env python3
import re
import sys

if __name__ == "__main__":
    if len(sys.argv) != 2:
        print(f"Usage: {sys.argv[0]} <file_path>")
        sys.exit(1)
    file_path = sys.argv[1]
    with open(file_path, 'r') as f:
        lines = f.readlines()

    summary_header = None
    pattern = re.compile(r'^(.+?)\s{2,}([\d\.]+)\s{2,}([\d\.]+)\s{2,}([\d\.]+)\s{2,}([\d\.]+)$')

    data = {}

    for line in lines:
        line = line.rstrip('\n')
        if summary_header is None and line.startswith("SUMMARY rate"):
            summary_header = line
        match = pattern.match(line.strip())
        if match:
            operation = match.group(1).strip()
            max_val = float(match.group(2))
            min_val = float(match.group(3))
            mean_val = float(match.group(4))
            if operation not in data:
                data[operation] = {'max_list': [], 'min_list': [], 'mean_list': []}
            data[operation]['max_list'].append(max_val)
            data[operation]['min_list'].append(min_val)
            data[operation]['mean_list'].append(mean_val)

    aggregated = []
    for op, values in data.items():
        # max_of_max = max(values['max_list'])
        # min_of_min = min(values['min_list'])
        # avg_of_mean = sum(values['mean_list']) / len(values['mean_list'])
        max_of_max = sum(values['max_list'])
        min_of_min = sum(values['min_list'])
        avg_of_mean = sum(values['mean_list'])
        aggregated.append({
            'Operation': op,
            'Max_of_Max': max_of_max,
            'Min_of_Min': min_of_min,
            'Avg_of_Mean': avg_of_mean
        })

    print(summary_header)
    print(f"{'Operation':<25} {'Max':>13} {'Min':>13} {'Mean':>13}")
    print(f"{'-'*25} {'-'*13} {'-'*13} {'-'*13}")

    for item in aggregated:
        op    = item['Operation']
        maxv  = item['Max_of_Max']
        minv  = item['Min_of_Min']
        meanv = item['Avg_of_Mean']
        print(f"{op:<25} {maxv:>13.3f} {minv:>13.3f} {meanv:>13.3f}")
