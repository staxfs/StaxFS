#!/usr/bin/env python3
"""Offline verifier for CXLPersistence SSD backing files (cross-meta view).

Usage:
    verify_persistence.py <flash_dir>

"""
import os, struct, sys, glob, re
from collections import defaultdict

SLOT = 128
PAGE = 4096

# include/common/metadata_types.h
INODE_FMT = "<Q Q I I I I Q Q Q Q q q q"  # 88 bytes
assert struct.calcsize(INODE_FMT) == 88

# include/server/dent_region.h
DIRPAGE_HDR = "<Q Q H H H I"              # 26 bytes packed
DIRPAGE_HDR_SIZE = struct.calcsize(DIRPAGE_HDR)
assert DIRPAGE_HDR_SIZE == 26

DENT_HDR = "<Q Q H B B"                   # 20 bytes packed
DENT_HDR_SIZE = struct.calcsize(DENT_HDR)
assert DENT_HDR_SIZE == 20

INODE_ID_RANGE = 50  # metadata_types.h


def human(n):
    for u in ("B", "K", "M", "G"):
        if n < 1024:
            return f"{n:.1f}{u}"
        n /= 1024
    return f"{n:.1f}T"


def owner_meta(inode_id):
    return inode_id >> INODE_ID_RANGE


def scan_inode_file(path):
    """Return {inode_id: (nlink, mode, uid, gid, size, ctime, slot_off)}."""
    out = {}
    apparent = os.path.getsize(path)
    on_disk = os.stat(path).st_blocks * 512
    with open(path, "rb") as f:
        data = f.read()
    for off in range(0, len(data), SLOT):
        rec = data[off:off + 88]
        if len(rec) < 88:
            break
        (id_, nlink, mode, extra, uid, gid,
         rdev, size, blksize, blocks, atime, mtime, ctime) = struct.unpack(INODE_FMT, rec)
        if id_ == 0:
            continue
        out[id_] = (nlink, mode, uid, gid, size, ctime, off)
    return out, apparent, on_disk


def scan_dent_file(path):
    """Return:
        records:    list of (parent_id, name, inode_id, version, type, is_tomb)
        page_stats: (pages_used, pages_total, creates, tombs)
    """
    apparent = os.path.getsize(path)
    on_disk = os.stat(path).st_blocks * 512
    records = []
    pages_total = 0
    pages_used = 0
    create_count = 0
    tomb_count = 0

    with open(path, "rb") as f:
        data = f.read()
    for poff in range(0, len(data), PAGE):
        page = data[poff:poff + PAGE]
        if len(page) < DIRPAGE_HDR_SIZE:
            break
        pages_total += 1
        dir_id, next_off, used, ent_cnt, live_cnt, cpseq = struct.unpack(
            DIRPAGE_HDR, page[:DIRPAGE_HDR_SIZE])
        if dir_id == 0 and used == 0 and ent_cnt == 0:
            continue
        pages_used += 1
        pos = DIRPAGE_HDR_SIZE
        end = DIRPAGE_HDR_SIZE + used
        seen = 0
        while pos + DENT_HDR_SIZE <= end and seen < ent_cnt:
            inode_id, version, reclen, etype, flags = struct.unpack(
                DENT_HDR, page[pos:pos + DENT_HDR_SIZE])
            if reclen < DENT_HDR_SIZE or pos + reclen > end:
                break
            name = page[pos + DENT_HDR_SIZE:pos + reclen].rstrip(b"\x00").decode(
                "utf-8", errors="replace")
            is_tomb = bool(flags & 1)
            records.append((dir_id, name, inode_id, version, etype, is_tomb))
            if is_tomb:
                tomb_count += 1
            else:
                create_count += 1
            pos += reclen
            seen += 1
    return records, apparent, on_disk, (pages_used, pages_total, create_count, tomb_count)


def main():
    if len(sys.argv) != 2:
        print(__doc__)
        sys.exit(1)
    flash_dir = sys.argv[1]
    metas = set()
    for p in glob.glob(os.path.join(flash_dir, "Meta*_Inode")):
        m = re.match(r"Meta(\d+)_Inode", os.path.basename(p))
        if m:
            metas.add(int(m.group(1)))
    metas = sorted(metas)

    # Phase 1: load all inode files (per-meta) and all dent records
    # (pooled globally, but tagged with source meta for reporting).
    per_meta_inodes = {}           # meta_id -> {inode_id: tuple}
    per_meta_dent_stats = {}       # meta_id -> (pages_used, pages_total, creates, tombs, nrecs)
    all_dent_records = []          # list of (src_meta, parent_id, name, iid, ver, etype, tomb)

    for n in metas:
        ipath = os.path.join(flash_dir, f"Meta{n}_Inode")
        dpath = os.path.join(flash_dir, f"Meta{n}_Dent")
        inode_tbl, ia, ib = scan_inode_file(ipath)
        recs, da, db, dstats = scan_dent_file(dpath)
        per_meta_inodes[n] = inode_tbl
        per_meta_dent_stats[n] = (*dstats, len(recs))
        for (dir_id, name, iid, ver, etype, tomb) in recs:
            all_dent_records.append((n, dir_id, name, iid, ver, etype, tomb))

        pages_used, pages_total, creates, tombs = dstats
        nlink_pos = sum(1 for v in inode_tbl.values() if v[0] > 0)
        nlink_zero = len(inode_tbl) - nlink_pos
        print(f"=== Meta {n} ===")
        print(f"  inode file: apparent={human(ia)} on_disk={human(ib)}  "
              f"slots_nonzero={len(inode_tbl)}  "
              f"nlink>0={nlink_pos}  soft_tombstone={nlink_zero}")
        print(f"  dent  file: apparent={human(da)} on_disk={human(db)}  "
              f"pages_used={pages_used}/{pages_total}  "
              f"creates={creates}  tombs={tombs}  records={len(recs)}")

    # Build global inode table: inode_id -> (nlink, owner_meta_seen, slot_off)
    # Also sanity-check ownership: an inode record should only appear on the
    # meta indicated by inode_id >> INODE_ID_RANGE.
    global_inodes = {}
    ownership_violations = []
    for n, tbl in per_meta_inodes.items():
        for iid, v in tbl.items():
            expected_owner = owner_meta(iid)
            if expected_owner != n:
                ownership_violations.append((n, iid, expected_owner))
            # If the same inode appears on multiple metas, keep the
            # entry from its expected owner; report anomaly if nlink
            # disagrees.
            if iid in global_inodes:
                old = global_inodes[iid]
                # Prefer the owner-meta record if we have one.
                if old[-1] == expected_owner:
                    continue
                if n == expected_owner:
                    global_inodes[iid] = (*v, n)
                    continue
                # Neither is owner — keep whichever is newer by ctime.
                if v[5] > old[5]:
                    global_inodes[iid] = (*v, n)
            else:
                global_inodes[iid] = (*v, n)

    # Phase 2: global cross-meta merge by (parent_id, name) via max version_.
    print()
    print("=== Global cross-meta merge ===")
    print(f"  total raw dent records (all metas): {len(all_dent_records)}")

    merged = {}  # (pid, name) -> (inode_id, version, etype, is_tomb, src_meta)
    for (src, pid, name, iid, ver, etype, tomb) in all_dent_records:
        key = (pid, name)
        cur = merged.get(key)
        if cur is None or cur[1] < ver:
            merged[key] = (iid, ver, etype, tomb, src)

    live_names = {k: v for k, v in merged.items() if not v[3]}
    dead_names = {k: v for k, v in merged.items() if v[3]}
    print(f"  distinct (parent_id, name) tuples: {len(merged)}")
    print(f"    globally live (winner is PUT): {len(live_names)}")
    print(f"    globally dead (winner is TOMB): {len(dead_names)}")

    # Tuples whose winner came from a different meta than the losing event:
    # proves the cross-meta merge is load-bearing.
    cross_meta_resolved = 0
    # group all raw records by key
    by_key = defaultdict(list)
    for (src, pid, name, iid, ver, etype, tomb) in all_dent_records:
        by_key[(pid, name)].append((src, ver, tomb))
    for key, rows in by_key.items():
        srcs = {r[0] for r in rows}
        if len(srcs) > 1:
            cross_meta_resolved += 1
    print(f"  (pid, name) tuples with records on >1 meta: {cross_meta_resolved}")

    # Phase 3: consistency checks against the GLOBAL view.
    print()
    print("=== Consistency cross-check (global view) ===")

    # Check 1: every globally-live name must point at an inode with nlink>0
    # on its owner meta.
    missing_inode = []
    referenced = set()
    for (pid, name), (iid, ver, etype, tomb, src) in live_names.items():
        referenced.add(iid)
        owner = owner_meta(iid)
        slot = per_meta_inodes.get(owner, {}).get(iid)
        if slot is None:
            missing_inode.append((pid, name, iid, src, f"no slot on owner meta{owner}"))
        elif slot[0] == 0:
            missing_inode.append((pid, name, iid, src, f"owner meta{owner} slot has nlink=0"))

    # Check 2: every nlink>0 inode must be referenced by some globally-live
    # name (except root id=2 and the per-meta pseudo-parent id=1).
    orphan_inodes = []
    for n, tbl in per_meta_inodes.items():
        for iid, v in tbl.items():
            if v[0] == 0:
                continue
            if iid == 1 or iid == 2:
                continue
            if iid not in referenced:
                orphan_inodes.append((n, iid, v))

    # Check 3: every globally-dead name's child inode should NOT have
    # nlink>0 on its owner meta (otherwise a TOMB won but the inode is
    # still alive — usually OK if the link count dropped via another name,
    # so only warn).
    stale_tomb = []
    for (pid, name), (iid, ver, etype, tomb, src) in dead_names.items():
        owner = owner_meta(iid)
        slot = per_meta_inodes.get(owner, {}).get(iid)
        if slot is not None and slot[0] > 0:
            # Check whether the inode is kept alive by some other live name.
            alive_elsewhere = any(
                live_iid == iid
                for (lpid, lname), (live_iid, *_rest) in live_names.items()
            )
            if not alive_elsewhere:
                stale_tomb.append((pid, name, iid, src))

    if ownership_violations:
        print(f"  !! OWNERSHIP_VIOLATION: {len(ownership_violations)} inodes on wrong meta")
        for n, iid, expected in ownership_violations[:10]:
            print(f"      inode={iid} on meta{n} but owner should be meta{expected}")
        if len(ownership_violations) > 10:
            print(f"      ... and {len(ownership_violations) - 10} more")
    else:
        print("  OK  every inode slot lives on its owner meta")

    if missing_inode:
        print(f"  !! LIVE_DENT_BUT_NO_INODE: {len(missing_inode)} records")
        for pid, name, iid, src, why in missing_inode[:10]:
            print(f"      pid={pid} name={name!r} child_inode={iid} "
                  f"winner_from=meta{src} ({why})")
        if len(missing_inode) > 10:
            print(f"      ... and {len(missing_inode) - 10} more")
    else:
        print("  OK  every globally-live dent has an nlink>0 inode on its owner meta")

    if orphan_inodes:
        print(f"  !! NLINK_POS_BUT_NO_DENT: {len(orphan_inodes)} inodes")
        for meta, iid, v in orphan_inodes[:10]:
            nlink, mode, uid, gid, size, ctime, off = v
            print(f"      on meta{meta} inode={iid} nlink={nlink} "
                  f"mode={oct(mode)} slot_off={off}")
        if len(orphan_inodes) > 10:
            print(f"      ... and {len(orphan_inodes) - 10} more")
    else:
        print("  OK  every nlink>0 inode is referenced by some globally-live dent")

    if stale_tomb:
        print(f"  !! STALE_TOMB: {len(stale_tomb)} dead names whose child inode is still alive with no other dent")
        for pid, name, iid, src in stale_tomb[:10]:
            print(f"      pid={pid} name={name!r} child_inode={iid} winner_from=meta{src}")
        if len(stale_tomb) > 10:
            print(f"      ... and {len(stale_tomb) - 10} more")
    else:
        print("  OK  no dangling dead dents")


if __name__ == "__main__":
    main()
