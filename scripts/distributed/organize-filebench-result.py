#!/bin/env python3
import re
import sys

if len(sys.argv) != 2:
    print("Usage: python3 script.py <logfile>")
    sys.exit(1)

file_path = sys.argv[1]

with open(file_path, 'r') as f:
    lines = f.readlines()

run_times = []

ops_data = {}

io_summary = {
    'sum_total_ops': 0.0,
    'sum_ops_s': 0.0,
    'sum_rd': 0.0,
    'sum_wr': 0.0,
    'sum_mb_s': 0.0,
    'sum_ms_op': 0.0,
    'count': 0
}

i = 0
n = len(lines)

while i < n:
    line = lines[i].strip()

    m_run = re.match(r'^\S+:\s*Run took\s*([\d\.]+)\s*seconds', line)
    if m_run:
        run_times.append(int(m_run.group(1)))
        i += 1
        while i < n and lines[i].strip() and "IO Summary" not in lines[i]:
            op_line = lines[i].strip()
            # statfile1            752871ops    12547ops/s   0.0mb/s    0.145ms/op [0.020ms - 30.431ms]
            m_op = re.match(r'^(\S+)\s+([\d\.]+)ops\s+([\d\.]+)ops/s\s+([\d\.]+)mb/s\s+([\d\.]+)ms/op', op_line)
            if m_op:
                name = m_op.group(1)
                ops = float(m_op.group(2))
                ops_s = float(m_op.group(3))
                mb_s = float(m_op.group(4))
                ms_op = float(m_op.group(5))
                if name not in ops_data:
                    ops_data[name] = {
                        'sum_ops': 0.0,
                        'sum_ops_s': 0.0,
                        'sum_mb_s': 0.0,
                        'sum_ms_op': 0.0,
                        'count': 0
                    }
                ops_data[name]['sum_ops'] += ops
                ops_data[name]['sum_ops_s'] += ops_s
                ops_data[name]['sum_mb_s'] += mb_s
                ops_data[name]['sum_ms_op'] += ms_op
                ops_data[name]['count'] += 1
            i += 1

        # IO Summary
        if i < n and "IO Summary" in lines[i]:
            io_line = lines[i].strip()
            m_io = re.search(
                    r'IO Summary:\s*([\d\.]+)\s*ops\s+([\d\.]+)\s*ops/s\s+([\d\.]+)\s*/\s*([\d\.]+)\s*rd/wr\s+([\d\.]+)\s*mb/s\s+([\d\.]+)\s*ms/op',
                    io_line
                )
            if m_io:
                total_ops = float(m_io.group(1))
                ops_s = float(m_io.group(2))
                rd = float(m_io.group(3))
                wr = float(m_io.group(4))
                mb_s = float(m_io.group(5))
                ms_op = float(m_io.group(6))
                io_summary['sum_total_ops'] += total_ops
                io_summary['sum_ops_s'] += ops_s
                io_summary['sum_rd'] += rd
                io_summary['sum_wr'] += wr
                io_summary['sum_mb_s'] += mb_s
                io_summary['sum_ms_op'] += ms_op
                io_summary['count'] += 1
    else:
        i += 1

print(f"Run took {run_times[0]} seconds...")

for name in reversed(ops_data.keys()):
    data = ops_data[name]
    count = data['count']
    avg_ms_op = data['sum_ms_op'] / count if count > 0 else 0.0
    print(f"{name:<20} "
          f"{data['sum_ops']:>9.0f}ops    "
          f"{data['sum_ops_s']:>8.0f}ops/s    "
          f"{data['sum_mb_s']:>7.1f}mb/s    "
          f"{avg_ms_op:>3.3f}ms/op")

cnt = io_summary['count']
if cnt > 0:
    avg_ms_io = io_summary['sum_ms_op'] / cnt
else:
    avg_ms_io = 0.0
print(f"IO Summary: {io_summary['sum_total_ops']:.0f} ops "
      f"{io_summary['sum_ops_s']:.3f} ops/s "
      f"{io_summary['sum_rd']:.0f}/{io_summary['sum_wr']:.0f} rd/wr "
      f"{io_summary['sum_mb_s']:.1f}mb/s "
      f"{avg_ms_io:.3f}ms/op")
