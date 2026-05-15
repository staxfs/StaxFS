#!/usr/bin/env python3
"""Aggregate HashtableStats windows from meta logs and compare hint vs no-hint.

Usage: analyze_hint.py <log1> [log2 ...]

Each log can contain many "── HashtableStats ──" windows (one per PrintSpace
tick). Counters reset each tick, so summing them gives the total work.

The stats cover four op groups — find, insert, update, erase — each with its
own call count, phase 1/2/3 breakdown (where applicable), and raw CXL traffic
counters. Files are grouped by TAG_HINT state (ON/OFF) and reported as a
single aggregate per group. When a group has more than one file, a compact
per-file table is appended so per-MDS variance stays visible. If both ON and
OFF are passed, a per-op delta section is printed at the end.
"""
import os
import re
import sys
from collections import defaultdict

BUILD_RE = re.compile(r"build: TAG_HINT=(ON|OFF)")
HEADER_RE = re.compile(r"── HashtableStats ──")
# Matches one "key = integer" per line; decimal-valued derived rows (bytes/op)
# do not match and are dropped — we recompute them from the integer totals.
KV_RE = re.compile(r"\s+([a-z_0-9]+)\s*=\s*(-?\d+)\s*$")

# Op groups rendered by report(). Each group has its own phase layout.
FIND_KEYS = [
    ("calls", "{op}_calls"),
    ("hits", "{op}_hits"),
    ("misses", "{op}_misses"),
    ("cand_probes", "{op}_cand_probes"),
    ("phase1_hint_neg", "{op}_phase1_hint_neg"),
    ("phase1_hint_pos", "{op}_phase1_hint_pos"),
    ("phase1_found", "{op}_phase1_found"),
    ("phase1_false_pos", "{op}_phase1_false_pos"),
    ("phase2_reads", "{op}_phase2_reads"),
    ("phase2_found", "{op}_phase2_found"),
    ("phase3_retries", "{op}_phase3_retries"),
    ("cxl_hr_reads", "{op}_cxl_hr_reads"),
    ("cxl_hr_writes", "{op}_cxl_hr_writes"),
    ("cxl_val_reads", "{op}_cxl_val_reads"),
    ("cxl_val_writes", "{op}_cxl_val_writes"),
]

INSERT_KEYS = [
    ("calls", "insert_calls"),
    ("hits", "insert_hits"),
    ("misses", "insert_misses"),
    ("cand_probes", "insert_cand_probes"),
    ("pass1_reads", "insert_pass1_reads"),
    ("pass1_placed", "insert_pass1_placed"),
    ("pass2_reads", "insert_pass2_reads"),
    ("pass2_placed", "insert_pass2_placed"),
    ("cuckoo_calls", "insert_cuckoo_calls"),
    ("cuckoo_placed", "insert_cuckoo_placed"),
    ("cxl_hr_reads", "insert_cxl_hr_reads"),
    ("cxl_hr_writes", "insert_cxl_hr_writes"),
    ("cxl_val_reads", "insert_cxl_val_reads"),
    ("cxl_val_writes", "insert_cxl_val_writes"),
]

HINT_KEYS = [
    "hint_set_insert",
    "hint_set_migrate",
    "hint_set_populate_find",
    "hint_set_populate_update",
    "hint_clear_erase",
]


def parse(path):
    """Return (tag_hint_state, {counter_name: total_across_windows})."""
    totals = defaultdict(int)
    tag_state = None
    in_window = False
    with open(path, "r", errors="replace") as f:
        for line in f:
            if HEADER_RE.search(line):
                in_window = True
                continue
            if not in_window:
                continue
            mb = BUILD_RE.search(line)
            if mb:
                tag_state = mb.group(1)
                continue
            # strip log prefix "[...] [I] [pid ...] [.h:NN] "
            idx = line.rfind("] ")
            body = line[idx + 2 :] if idx >= 0 else line
            m = KV_RE.match(body)
            if m:
                totals[m.group(1)] += int(m.group(2))
    return tag_state, totals


def op_metrics(totals, op):
    """Compute derived per-op metrics for find/update/erase.

    Returns a dict with call/hit/miss counts, hint effect ratios, and a
    bytes/op figure that rolls up every CXL read and write on the op path.
    """
    calls = totals.get(f"{op}_calls", 0)
    hits = totals.get(f"{op}_hits", 0)
    miss = totals.get(f"{op}_misses", 0)
    cand = totals.get(f"{op}_cand_probes", 0)
    hn = totals.get(f"{op}_phase1_hint_neg", 0)
    hp = totals.get(f"{op}_phase1_hint_pos", 0)
    hf = totals.get(f"{op}_phase1_found", 0)
    hfp = totals.get(f"{op}_phase1_false_pos", 0)
    p2r = totals.get(f"{op}_phase2_reads", 0)
    p2f = totals.get(f"{op}_phase2_found", 0)
    p3 = totals.get(f"{op}_phase3_retries", 0)
    hr_r = totals.get(f"{op}_cxl_hr_reads", 0)
    hr_w = totals.get(f"{op}_cxl_hr_writes", 0)
    vr = totals.get(f"{op}_cxl_val_reads", 0)
    vw = totals.get(f"{op}_cxl_val_writes", 0)
    bytes_total = (hr_r + hr_w) * 64 + (vr + vw) * 128
    return {
        "calls": calls,
        "hits": hits,
        "misses": miss,
        "cand": cand,
        "phase1_hint_neg": hn,
        "phase1_hint_pos": hp,
        "phase1_found": hf,
        "phase1_false_pos": hfp,
        "phase2_reads": p2r,
        "phase2_found": p2f,
        "phase3_retries": p3,
        "hr_reads": hr_r,
        "hr_writes": hr_w,
        "val_reads": vr,
        "val_writes": vw,
        "hit_rate": hits / calls if calls else 0.0,
        "probes_per_op": cand / calls if calls else 0.0,
        "hr_reads_per_op": hr_r / calls if calls else 0.0,
        "val_reads_per_op": vr / calls if calls else 0.0,
        "bytes_per_op": bytes_total / calls if calls else 0.0,
        # Phase-1 effectiveness: fraction of candidate buckets skipped because
        # the hint said no. Directly maps to HR reads avoided.
        "hint_save_ratio": hn / cand if cand else 0.0,
        # Quality of phase-1 positive probes.
        "hint_fp_rate": hfp / hp if hp else 0.0,
        # Share of real hits that resolved in phase 1 (warm hint).
        "phase1_first_hit": hf / hits if hits else 0.0,
    }


def insert_metrics(totals):
    calls = totals.get("insert_calls", 0)
    hits = totals.get("insert_hits", 0)
    miss = totals.get("insert_misses", 0)
    cand = totals.get("insert_cand_probes", 0)
    p1r = totals.get("insert_pass1_reads", 0)
    p1p = totals.get("insert_pass1_placed", 0)
    p2r = totals.get("insert_pass2_reads", 0)
    p2p = totals.get("insert_pass2_placed", 0)
    cc = totals.get("insert_cuckoo_calls", 0)
    cp = totals.get("insert_cuckoo_placed", 0)
    hr_r = totals.get("insert_cxl_hr_reads", 0)
    hr_w = totals.get("insert_cxl_hr_writes", 0)
    vr = totals.get("insert_cxl_val_reads", 0)
    vw = totals.get("insert_cxl_val_writes", 0)
    bytes_total = (hr_r + hr_w) * 64 + (vr + vw) * 128
    return {
        "calls": calls,
        "hits": hits,
        "misses": miss,
        "cand": cand,
        "pass1_reads": p1r,
        "pass1_placed": p1p,
        "pass2_reads": p2r,
        "pass2_placed": p2p,
        "cuckoo_calls": cc,
        "cuckoo_placed": cp,
        "hr_reads": hr_r,
        "hr_writes": hr_w,
        "val_reads": vr,
        "val_writes": vw,
        "hit_rate": hits / calls if calls else 0.0,
        "probes_per_op": cand / calls if calls else 0.0,
        "hr_reads_per_op": hr_r / calls if calls else 0.0,
        "hr_writes_per_op": hr_w / calls if calls else 0.0,
        "val_writes_per_op": vw / calls if calls else 0.0,
        "bytes_per_op": bytes_total / calls if calls else 0.0,
        "pass1_hit_rate": p1p / hits if hits else 0.0,
        "cuckoo_success_rate": cp / cc if cc else 0.0,
    }


def fmt_row(label, val, width=26):
    return f"  {label:<{width}} {val}"


def report_hint_op(op, m):
    """Render one hint-capable op block (find / update / erase)."""
    print(f"  ── {op} ──")
    print(fmt_row(f"{op}_calls", f"{m['calls']:>14,}"))
    print(fmt_row(f"{op}_hits", f"{m['hits']:>14,}"))
    print(fmt_row(f"{op}_misses", f"{m['misses']:>14,}"))
    print(fmt_row("hit_rate", f"{m['hit_rate']:.4f}"))
    print(fmt_row("probes/op", f"{m['probes_per_op']:.3f}"))
    print(fmt_row("phase1_hint_neg", f"{m['phase1_hint_neg']:>14,}"))
    print(fmt_row("phase1_hint_pos", f"{m['phase1_hint_pos']:>14,}"))
    print(fmt_row("phase1_found", f"{m['phase1_found']:>14,}"))
    print(fmt_row("phase1_false_pos", f"{m['phase1_false_pos']:>14,}"))
    print(fmt_row("phase2_reads", f"{m['phase2_reads']:>14,}"))
    print(fmt_row("phase2_found", f"{m['phase2_found']:>14,}"))
    print(fmt_row("phase3_retries", f"{m['phase3_retries']:>14,}"))
    print(fmt_row("cxl_hr_reads", f"{m['hr_reads']:>14,}"))
    print(fmt_row("cxl_hr_writes", f"{m['hr_writes']:>14,}"))
    print(fmt_row("cxl_val_reads", f"{m['val_reads']:>14,}"))
    print(fmt_row("cxl_val_writes", f"{m['val_writes']:>14,}"))
    print(fmt_row("hr_reads/op", f"{m['hr_reads_per_op']:.3f}"))
    print(fmt_row("val_reads/op", f"{m['val_reads_per_op']:.3f}"))
    print(fmt_row("bytes/op", f"{m['bytes_per_op']:.1f} B"))
    print(fmt_row("hint_save_ratio", f"{m['hint_save_ratio']:.4f}"))
    print(fmt_row("hint_fp_rate", f"{m['hint_fp_rate']:.4f}"))
    print(fmt_row("phase1_first_hit_rate", f"{m['phase1_first_hit']:.4f}"))


def report_insert(m):
    print("  ── insert ──")
    print(fmt_row("insert_calls", f"{m['calls']:>14,}"))
    print(fmt_row("insert_hits", f"{m['hits']:>14,}"))
    print(fmt_row("insert_misses", f"{m['misses']:>14,}"))
    print(fmt_row("hit_rate", f"{m['hit_rate']:.4f}"))
    print(fmt_row("probes/op", f"{m['probes_per_op']:.3f}"))
    print(fmt_row("pass1_reads", f"{m['pass1_reads']:>14,}"))
    print(fmt_row("pass1_placed", f"{m['pass1_placed']:>14,}"))
    print(fmt_row("pass2_reads", f"{m['pass2_reads']:>14,}"))
    print(fmt_row("pass2_placed", f"{m['pass2_placed']:>14,}"))
    print(fmt_row("cuckoo_calls", f"{m['cuckoo_calls']:>14,}"))
    print(fmt_row("cuckoo_placed", f"{m['cuckoo_placed']:>14,}"))
    print(fmt_row("cxl_hr_reads", f"{m['hr_reads']:>14,}"))
    print(fmt_row("cxl_hr_writes", f"{m['hr_writes']:>14,}"))
    print(fmt_row("cxl_val_reads", f"{m['val_reads']:>14,}"))
    print(fmt_row("cxl_val_writes", f"{m['val_writes']:>14,}"))
    print(fmt_row("hr_reads/op", f"{m['hr_reads_per_op']:.3f}"))
    print(fmt_row("hr_writes/op", f"{m['hr_writes_per_op']:.3f}"))
    print(fmt_row("val_writes/op", f"{m['val_writes_per_op']:.3f}"))
    print(fmt_row("bytes/op", f"{m['bytes_per_op']:.1f} B"))
    print(fmt_row("pass1_hit_rate", f"{m['pass1_hit_rate']:.4f}"))
    print(fmt_row("cuckoo_success_rate", f"{m['cuckoo_success_rate']:.4f}"))


def report(tag, entries, derived):
    files = [p for p, _ in entries]
    n = len(files)
    tag_label = tag if tag else "UNKNOWN"
    plural = "file" if n == 1 else "files"
    print(f"══ TAG_HINT={tag_label}  ({n} {plural}, aggregate) ══")
    if n == 1:
        print(f"  source: {os.path.basename(files[0])}")
    else:
        print("  sources: " + ", ".join(os.path.basename(p) for p in files))

    report_hint_op("find", derived["find"])
    report_insert(derived["insert"])
    report_hint_op("update", derived["update"])
    report_hint_op("erase", derived["erase"])

    print("  ── hint maintenance ──")
    for k in HINT_KEYS:
        print(fmt_row(k, f"{derived['hint'][k]:>14,}"))

    # Per-file breakdown — only when the group has more than one file.
    # Show only the bytes/op bottom line for each op to keep it compact.
    if n > 1:
        print()
        print("  per-file bytes/op (find / insert / update / erase):")
        name_w = max(len(os.path.basename(p)) for p in files)
        name_w = max(name_w, len("file"))
        print(
            f"    {'file':<{name_w}}  {'find':>10}  {'insert':>10}"
            f"  {'update':>10}  {'erase':>10}"
        )
        for path, totals in entries:
            fm = op_metrics(totals, "find")
            im = insert_metrics(totals)
            um = op_metrics(totals, "update")
            em = op_metrics(totals, "erase")
            print(
                f"    {os.path.basename(path):<{name_w}}"
                f"  {fm['bytes_per_op']:>8.1f} B"
                f"  {im['bytes_per_op']:>8.1f} B"
                f"  {um['bytes_per_op']:>8.1f} B"
                f"  {em['bytes_per_op']:>8.1f} B"
            )
    print()


def delta_hint_op(op, on, off):
    def row(label, a, b, fmt=".3f"):
        d = a - b
        rel = (d / b * 100.0) if b else 0.0
        print(
            f"  {label:<20} {a:>14{fmt}}  {b:>14{fmt}}"
            f"  {d:>+14{fmt}}  {rel:>+7.2f}%"
        )

    print(f"  ── {op} ──")
    row("calls", on["calls"], off["calls"], ",.0f")
    row("hr_reads/op", on["hr_reads_per_op"], off["hr_reads_per_op"])
    row("val_reads/op", on["val_reads_per_op"], off["val_reads_per_op"])
    row("bytes/op", on["bytes_per_op"], off["bytes_per_op"], ".1f")
    if on["calls"] and off["calls"]:
        saved = off["hr_reads_per_op"] - on["hr_reads_per_op"]
        total = saved * on["calls"]
        print(
            f"    hr reads saved per op : {saved:+.3f}"
            f"  (total {total:+,.0f}, ~{saved * 64:+.1f} B/op)"
        )


def delta_insert(on, off):
    def row(label, a, b, fmt=".3f"):
        d = a - b
        rel = (d / b * 100.0) if b else 0.0
        print(
            f"  {label:<20} {a:>14{fmt}}  {b:>14{fmt}}"
            f"  {d:>+14{fmt}}  {rel:>+7.2f}%"
        )

    print("  ── insert ──")
    row("calls", on["calls"], off["calls"], ",.0f")
    row("hr_reads/op", on["hr_reads_per_op"], off["hr_reads_per_op"])
    row("hr_writes/op", on["hr_writes_per_op"], off["hr_writes_per_op"])
    row("val_writes/op", on["val_writes_per_op"], off["val_writes_per_op"])
    row("bytes/op", on["bytes_per_op"], off["bytes_per_op"], ".1f")


def delta(on_all, off_all):
    print("════════════ HINT ON vs OFF (aggregate) ════════════")
    print(
        f"  {'metric':<20} {'HINT ON':>14}  {'HINT OFF':>14}"
        f"  {'delta':>14}  {'rel':>8}"
    )
    delta_hint_op("find", on_all["find"], off_all["find"])
    delta_insert(on_all["insert"], off_all["insert"])
    delta_hint_op("update", on_all["update"], off_all["update"])
    delta_hint_op("erase", on_all["erase"], off_all["erase"])


def derive_all(totals):
    return {
        "find": op_metrics(totals, "find"),
        "insert": insert_metrics(totals),
        "update": op_metrics(totals, "update"),
        "erase": op_metrics(totals, "erase"),
        "hint": {k: totals.get(k, 0) for k in HINT_KEYS},
    }


def main():
    if len(sys.argv) < 2:
        print("usage: analyze_hint.py <log1> [log2 ...]")
        sys.exit(1)

    # Parse every input, then bucket by TAG_HINT state.
    groups = defaultdict(list)  # tag -> [(path, totals), ...]
    for p in sys.argv[1:]:
        tag, totals = parse(p)
        groups[tag].append((p, totals))

    # Derive and print per-group aggregate. Stable order: ON, OFF, unknown.
    order = ["ON", "OFF", None]
    derived_by_tag = {}
    for tag in order:
        if tag not in groups:
            continue
        entries = groups[tag]
        merged = defaultdict(int)
        for _, totals in entries:
            for k, v in totals.items():
                merged[k] += v
        d = derive_all(merged)
        derived_by_tag[tag] = d
        report(tag, entries, d)

    # Cross-build delta when both ON and OFF were provided.
    if "ON" in derived_by_tag and "OFF" in derived_by_tag:
        delta(derived_by_tag["ON"], derived_by_tag["OFF"])


if __name__ == "__main__":
    main()
