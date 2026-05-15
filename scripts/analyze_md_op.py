#!/usr/bin/env python3
"""Aggregate md-op latency stats across multiple meta logs.

Usage: analyze_md_op.py <log1> [log2 ...]

Each log can contain many "── md-op profile ..." windows (one per
PrintSpace tick). We use the LAST window from each log — that's the
cumulative-since-start snapshot at run end. Per-log windows are summed
across files to give the cluster-wide totals.

Each window emits one machine-readable line per op:
    md-op-raw <Op> count=<N> total_ns=<T> max_ns=<M> hist=<b0,b1,...,b63>

where bucket k counts samples in [2^k, 2^(k+1)) ns. Percentiles are
recomputed from the merged histograms (bucket-midpoint approximation,
within ~50% relative error of true value).
"""
import os
import re
import sys
from collections import defaultdict

WINDOW_HEADER_RE = re.compile(r"── md-op profile ")
RAW_RE = re.compile(
    r"md-op-raw\s+(\S+)\s+count=(\d+)\s+total_ns=(\d+)\s+max_ns=(\d+)"
    r"\s+hist=([\d,]+)"
)

HIST_BUCKETS = 64


def parse_last_window(path):
    """Return {op: {count, total_ns, max_ns, hist[64]}} from the LAST
    window in the log. Empty dict if no window found."""
    last = {}
    cur = {}
    with open(path, "r", errors="replace") as f:
        for line in f:
            if WINDOW_HEADER_RE.search(line):
                if cur:
                    last = cur
                cur = {}
                continue
            m = RAW_RE.search(line)
            if not m:
                continue
            op = m.group(1)
            count = int(m.group(2))
            total_ns = int(m.group(3))
            max_ns = int(m.group(4))
            hist = [int(x) for x in m.group(5).split(",")]
            if len(hist) != HIST_BUCKETS:
                continue
            cur[op] = {
                "count": count,
                "total_ns": total_ns,
                "max_ns": max_ns,
                "hist": hist,
            }
    if cur:
        last = cur
    return last


def merge(per_file):
    """Sum count/total/hist; max for max. per_file: list of {op: stats}."""
    merged = defaultdict(
        lambda: {
            "count": 0,
            "total_ns": 0,
            "max_ns": 0,
            "hist": [0] * HIST_BUCKETS,
        }
    )
    for snap in per_file:
        for op, st in snap.items():
            m = merged[op]
            m["count"] += st["count"]
            m["total_ns"] += st["total_ns"]
            if st["max_ns"] > m["max_ns"]:
                m["max_ns"] = st["max_ns"]
            for k in range(HIST_BUCKETS):
                m["hist"][k] += st["hist"][k]
    return merged


def percentile_ns(hist, total, p):
    """Bucket-midpoint approximation. Walk buckets low → high and return
    the midpoint of the bucket where cumulative count first reaches the
    target percentile."""
    if total == 0:
        return 0
    target = max(1, min(total, int(round(total * p))))
    cum = 0
    for k in range(HIST_BUCKETS):
        cum += hist[k]
        if cum >= target:
            if k == 0:
                return 1
            # midpoint of [2^k, 2^(k+1)) = 3 * 2^(k-1)
            return 3 << (k - 1)
    return 0


# Stable display order (matches MDOpStep enum).
OP_ORDER = [
    "Unlink",
    "RemoveDir",
    "Access",
    "MakeDir",
    "Stat",
    "Rename",
    "Link",
    "Create",
]


def report(merged, files):
    print(f"══ md-op aggregate over {len(files)} log(s) ══")
    for p in files:
        print(f"  source: {os.path.basename(p)}")
    print()
    hdr = (
        f"  {'op':<10}  {'count':>12}  {'avg_us':>10}  {'p50_us':>10}  "
        f"{'p99_us':>10}  {'p999_us':>10}  {'p9999_us':>10}  {'max_us':>12}"
    )
    print(hdr)
    print("  " + "-" * (len(hdr) - 2))

    seen = set()
    rows = []
    for op in OP_ORDER:
        if op in merged:
            rows.append(op)
            seen.add(op)
    # Any unexpected op names (shouldn't happen) appended at the end.
    for op in sorted(merged.keys()):
        if op not in seen:
            rows.append(op)

    for op in rows:
        m = merged[op]
        cnt = m["count"]
        if cnt == 0:
            continue
        avg_us = m["total_ns"] / cnt / 1000.0
        p50 = percentile_ns(m["hist"], cnt, 0.50) / 1000.0
        p99 = percentile_ns(m["hist"], cnt, 0.99) / 1000.0
        p999 = percentile_ns(m["hist"], cnt, 0.999) / 1000.0
        p9999 = percentile_ns(m["hist"], cnt, 0.9999) / 1000.0
        max_us = m["max_ns"] / 1000.0
        print(
            f"  {op:<10}  {cnt:>12,}  {avg_us:>10.2f}  {p50:>10.2f}  "
            f"{p99:>10.2f}  {p999:>10.2f}  {p9999:>10.2f}  {max_us:>12.2f}"
        )


def main():
    if len(sys.argv) < 2:
        print("usage: analyze_md_op.py <log1> [log2 ...]", file=sys.stderr)
        sys.exit(1)
    files = sys.argv[1:]
    snaps = []
    for p in files:
        snap = parse_last_window(p)
        if not snap:
            print(f"warn: no md-op profile window found in {p}",
                  file=sys.stderr)
        snaps.append(snap)
    merged = merge(snaps)
    if not merged:
        print("error: no data in any log", file=sys.stderr)
        sys.exit(2)
    report(merged, files)


if __name__ == "__main__":
    main()
