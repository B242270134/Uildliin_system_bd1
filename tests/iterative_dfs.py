#!/usr/bin/env python3

from collections import defaultdict
import os

def get_processes():
    processes = {}

    for name in os.listdir("/proc"):
        if not name.isdigit():
            continue

        pid = int(name)

        try:
            with open(f"/proc/{pid}/stat") as f:
                data = f.read()

            end = data.rfind(")")
            comm = data[data.find("(") + 1:end]
            fields = data[end + 2:].split()

            state = fields[0]
            ppid = int(fields[1])

            processes[pid] = {
                "pid": pid,
                "ppid": ppid,
                "comm": comm,
                "state": state
            }

        except (OSError, ValueError, IndexError):
            continue

    return processes


def iterative_dfs(processes):
    children = defaultdict(list)

    for pid, process in processes.items():
        children[process["ppid"]].append(pid)

    for pids in children.values():
        pids.sort()

    roots = sorted(
        pid for pid, process in processes.items()
        if process["ppid"] not in processes
    )

    stack = [(pid, 0) for pid in reversed(roots)]
    visited = set()

    print(
        f"{'DEPTH':<8}"
        f"{'PID':<10}"
        f"{'PPID':<10}"
        f"{'STATE':<8}"
        f"COMMAND"
    )

    while stack:
        pid, depth = stack.pop()

        if pid in visited:
            continue

        visited.add(pid)

        process = processes[pid]

        print(
            f"{depth:<8}"
            f"{pid:<10}"
            f"{process['ppid']:<10}"
            f"{process['state']:<8}"
            f"{process['comm']}"
        )

        for child in reversed(children.get(pid, [])):
            stack.append((child, depth + 1))

    print(f"\nDFS visited {len(visited)} processes")


if __name__ == "__main__":
    processes = get_processes()
    iterative_dfs(processes)
