"""
Data-load utility: recursively copy a directory tree.

Exposes two public interfaces for external callers:

    load_serial(src, dst, chunk_size=8192)
    load_parallel(src, dst, max_workers=30, chunk_size=8192)

Both return the elapsed wall-clock time (in seconds).
"""

import argparse
import os
import sys
import time
import multiprocessing
from concurrent.futures import ProcessPoolExecutor, wait, FIRST_COMPLETED

multiprocessing.set_start_method('spawn', force=True)

DEFAULT_CHUNK_SIZE = 8 * 1024
DEFAULT_MAX_WORKERS = 30


# ---------------------------------------------------------------------------
# Internal helpers
# ---------------------------------------------------------------------------

def _copy_file(src_path: str, dst_path: str, chunk_size: int) -> None:
    fd_src = os.open(src_path, os.O_RDONLY)
    fd_dst = os.open(dst_path,
                     os.O_WRONLY | os.O_CREAT | os.O_TRUNC,
                     0o644)
    try:
        while True:
            buf = os.read(fd_src, chunk_size)
            if not buf:
                break
            os.write(fd_dst, buf)
    finally:
        os.close(fd_src)
        os.close(fd_dst)


def _serial_copy(src_path: str, dst_path: str, chunk_size: int) -> None:
    if os.path.isdir(src_path):
        os.makedirs(dst_path, exist_ok=True)
        for entry in os.listdir(src_path):
            _serial_copy(os.path.join(src_path, entry),
                         os.path.join(dst_path, entry),
                         chunk_size)
    else:
        _copy_file(src_path, dst_path, chunk_size)


def _copy_one_level(src_dir: str, dst_dir: str, chunk_size: int):
    """
    Copy only the files directly under ``src_dir``; return a list of
    ``(sub_src, sub_dst)`` sub-directories for the caller to schedule next.
    """
    next_dirs = []
    os.makedirs(dst_dir, exist_ok=True)
    for name in os.listdir(src_dir):
        src_path = os.path.join(src_dir, name)
        dst_path = os.path.join(dst_dir, name)
        if os.path.isdir(src_path):
            next_dirs.append((src_path, dst_path))
        else:
            _copy_file(src_path, dst_path, chunk_size)
    return next_dirs


# ---------------------------------------------------------------------------
# Public API
# ---------------------------------------------------------------------------

def load_serial(src: str, dst: str, chunk_size: int = DEFAULT_CHUNK_SIZE) -> float:
    """
    Recursively copy ``src`` into ``dst`` in a single process.

    Returns elapsed wall-clock time in seconds.
    """
    start = time.time()
    _serial_copy(src, dst, chunk_size)
    return time.time() - start


def load_parallel(src: str,
                  dst: str,
                  max_workers: int = DEFAULT_MAX_WORKERS,
                  chunk_size: int = DEFAULT_CHUNK_SIZE) -> float:
    """
    Recursively copy ``src`` into ``dst`` using a process pool, level by level.

    Each directory is handed to a worker which copies every regular file
    directly under it and returns its sub-directories; the sub-directories
    are then scheduled as new tasks. This keeps all workers busy without
    requiring a global directory walk up front.

    Returns elapsed wall-clock time in seconds.
    """
    start = time.time()

    with ProcessPoolExecutor(max_workers=max_workers) as exe:
        futures_map = {
            exe.submit(_copy_one_level, src, dst, chunk_size): (src, dst)
        }

        while futures_map:
            done_set, _ = wait(futures_map, return_when=FIRST_COMPLETED)
            for fut in done_set:
                cur_src, cur_dst = futures_map.pop(fut)
                try:
                    subdirs = fut.result()
                except Exception as e:
                    print(f"Error copying level {cur_src} -> {cur_dst}: {e}",
                          file=sys.stderr)
                    continue

                for sub_src, sub_dst in subdirs:
                    futures_map[
                        exe.submit(_copy_one_level, sub_src, sub_dst, chunk_size)
                    ] = (sub_src, sub_dst)

    return time.time() - start


# ---------------------------------------------------------------------------
# Backwards-compatible aliases (kept so older imports keep working)
# ---------------------------------------------------------------------------

def serial_copy(src_path: str, dst_path: str, chunk_size: int = DEFAULT_CHUNK_SIZE):
    _serial_copy(src_path, dst_path, chunk_size)


def parallel_copy_by_level(src_root: str, dst_root: str, max_workers: int = None):
    load_parallel(src_root, dst_root,
                  max_workers=max_workers or DEFAULT_MAX_WORKERS)


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def main(argv=None):
    parser = argparse.ArgumentParser(description="Recursively copy a directory tree.")
    parser.add_argument("src", help="source directory")
    parser.add_argument("dst", help="destination directory")
    parser.add_argument("--mode", choices=("serial", "parallel"), default="parallel",
                        help="copy mode (default: parallel)")
    parser.add_argument("--workers", type=int, default=DEFAULT_MAX_WORKERS,
                        help=f"parallel worker count (default: {DEFAULT_MAX_WORKERS})")
    parser.add_argument("--chunk-size", type=int, default=DEFAULT_CHUNK_SIZE,
                        help=f"read/write chunk size in bytes (default: {DEFAULT_CHUNK_SIZE})")
    args = parser.parse_args(argv)

    if args.mode == "serial":
        elapsed = load_serial(args.src, args.dst, chunk_size=args.chunk_size)
    else:
        elapsed = load_parallel(args.src, args.dst,
                                max_workers=args.workers,
                                chunk_size=args.chunk_size)

    print(f"Copy total elapsed time: {elapsed:.3f} s")


if __name__ == "__main__":
    main()
