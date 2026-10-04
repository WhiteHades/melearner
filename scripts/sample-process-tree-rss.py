#!/usr/bin/env python3
"""Run one command and record its Linux process-tree RSS at fixed intervals."""

import argparse
import csv
import os
import subprocess
import sys
import time


def process_table():
    table = {}
    for entry in os.scandir("/proc"):
        if not entry.name.isdigit():
            continue
        try:
            with open(f"/proc/{entry.name}/stat", encoding="ascii") as stat_file:
                fields = stat_file.read().rsplit(")", 1)[1].split()
            ppid = int(fields[1])
            with open(f"/proc/{entry.name}/status", encoding="ascii") as status_file:
                rss = next((int(line.split()[1]) for line in status_file if line.startswith("VmRSS:")), 0)
            with open(f"/proc/{entry.name}/comm", encoding="ascii") as comm_file:
                comm = comm_file.read().strip().replace(";", "_")
            table[int(entry.name)] = (ppid, rss, comm)
        except (FileNotFoundError, ProcessLookupError, PermissionError, ValueError, IndexError):
            continue
    return table


def tree_rss(root_pid, table):
    members = {root_pid}
    while True:
        children = {pid for pid, (ppid, _, _) in table.items() if ppid in members}
        if children <= members:
            break
        members.update(children)
    ordered = sorted(pid for pid in members if pid in table)
    rss = sum(table[pid][1] for pid in ordered)
    breakdown = ";".join(f"{pid}:{table[pid][2]}={table[pid][1]}" for pid in ordered)
    return rss, len(ordered), breakdown


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--csv", required=True)
    parser.add_argument("--interval", type=float, default=0.5)
    parser.add_argument("command", nargs=argparse.REMAINDER)
    args = parser.parse_args()
    command = args.command[1:] if args.command[:1] == ["--"] else args.command
    if not command or args.interval <= 0:
        parser.error("provide -- COMMAND and a positive interval")

    os.makedirs(os.path.dirname(os.path.abspath(args.csv)), mode=0o700, exist_ok=True)
    with open(args.csv, "w", newline="", encoding="utf-8") as output:
        writer = csv.writer(output)
        writer.writerow(("elapsed_ms", "tree_rss_kib", "processes", "rss_by_pid_kib"))
        child = subprocess.Popen(command)
        started = time.monotonic()
        peak = 0
        try:
            while child.poll() is None:
                rss, count, breakdown = tree_rss(child.pid, process_table())
                elapsed = round((time.monotonic() - started) * 1000)
                peak = max(peak, rss)
                writer.writerow((elapsed, rss, count, breakdown))
                output.flush()
                time.sleep(args.interval)
            rss, count, breakdown = tree_rss(child.pid, process_table())
            elapsed = round((time.monotonic() - started) * 1000)
            peak = max(peak, rss)
            writer.writerow((elapsed, rss, count, breakdown))
        except KeyboardInterrupt:
            child.terminate()
            raise
    print(f"RSS peak_tree_kib={peak} samples={max(0, sum(1 for _ in open(args.csv, encoding='utf-8')) - 1)}", file=sys.stderr)
    return child.returncode


if __name__ == "__main__":
    sys.exit(main())
