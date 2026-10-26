#!/usr/bin/env python3
"""
compare_ps.py - tasktree module-ийн гаралтыг шалгах тест.

1) DFS шалгалт (snapshot дангаараа): pre-order дараалал зөв эсэх
   - order 0 нь PID 0 (init_task), depth 0
   - task бүрийн графын эцэг (parent) одоогийн замд байна, depth = эцгийн depth + 1
2) LINEAR ↔ DFS бүрэн байдал (kernel нэг агшинд хоёуланг нь авдаг):
   for_each_process-оор олдсон процесс бүр DFS модонд байх ёстой.
   Thread-ээс fork хийсэн хүүхдийг алгасдаг DFS энэ шалгалтад унана.
3) ps-тэй харьцуулалт (--ps эсвэл --ps-file): PID олонлог, PPID / COMMAND / STATE

Хэрэглээ:
  python3 tests/compare_ps.py                                    # /proc/tasktree + ps
  python3 tests/compare_ps.py --file examples/qemu/snap_workload.json
  python3 tests/compare_ps.py --file snap.json --ps-file ps.txt    # хадгалсан ps гаралттай
  python3 tests/compare_ps.py --url http://127.0.0.1:8000/api/tasks --ps
"""
import argparse
import json
import subprocess
import sys
import urllib.request


def load(args):
    if args.file:
        with open(args.file, encoding="utf-8") as f:
            return json.load(f)
    if args.url:
        with urllib.request.urlopen(args.url, timeout=5) as r:
            return json.load(r)
    try:
        with open(args.proc, encoding="utf-8") as f:
            return json.load(f)
    except FileNotFoundError:
        sys.exit(f"[FAIL] {args.proc} алга. `sudo insmod src/kernel/tasktree.ko` хийнэ үү.")


def gparent(t):
    return t.get("parent", t["ppid"])


def check_dfs(snap):
    tasks = snap["tasks"]
    errors = []
    if snap.get("count") != len(tasks):
        errors.append(f"count={snap.get('count')} гэтэл tasks={len(tasks)}")
    if not tasks or tasks[0]["pid"] != 0 or tasks[0]["depth"] != 0:
        errors.append("эхний task нь PID 0, depth 0 биш")
    depth_of = {}
    stack = []     # одоогийн замын pid-ууд (root -> ...)
    pids = set()
    for i, t in enumerate(tasks):
        if t["order"] != i:
            errors.append(f"order {t['order']} != index {i}")
        if i == 0:
            stack, depth_of[0] = [0], 0
            continue
        if not t.get("thread"):
            if t["pid"] in pids:
                errors.append(f"PID {t['pid']} давхар гарсан")
            pids.add(t["pid"])
        par = gparent(t)
        while stack and stack[-1] != par:
            stack.pop()          # pre-order: эцэг заавал одоогийн замд байх ёстой
        if not stack:
            errors.append(f"PID {t['pid']} ({t['comm']}): эцэг {par} замд алга -> DFS дараалал буруу")
            stack = [par]
        elif par in depth_of and t["depth"] != depth_of[par] + 1:
            errors.append(f"PID {t['pid']}: depth {t['depth']} != эцгийн depth+1")
        if not t.get("thread"):
            depth_of[t["pid"]] = t["depth"]
            stack.append(t["pid"])
    return errors


def check_linear(snap):
    """LINEAR ба DFS нэг агшных тул процессын олонлог яг тэнцүү байх ёстой."""
    if "linear" not in snap:
        return None, "linear талбаргүй (хуучин формат) - алгасав"
    if snap.get("truncated"):
        return None, f"truncated=true (cap={snap.get('cap')}) - бүрэн байдлыг шалгахгүй"
    dfs = {t["pid"] for t in snap["tasks"] if not t.get("thread") and t["pid"] != 0}
    lin = set(snap["linear"])
    missing = sorted(lin - dfs)
    extra = sorted(dfs - lin)
    names = {t["pid"]: t["comm"] for t in snap["tasks"]}
    msg = (f"LINEAR {len(lin)} процесс, DFS {len(dfs)} процесс (+PID 0); "
           f"DFS-д дутуу: {len(missing)} {missing[:10]}, илүү: {len(extra)} {[ (p, names.get(p)) for p in extra[:10]]}")
    return (not missing and not extra), msg


def parse_ps(text):
    rows = {}
    for line in text.splitlines():
        parts = line.split(None, 3)
        if len(parts) == 4 and parts[0].isdigit():
            rows[int(parts[0])] = {"ppid": int(parts[1]), "stat": parts[2], "comm": parts[3].strip()}
    return rows


def ps_table(threads):
    cmd = ["ps", "-eLo" if threads else "-eo", "lwp,ppid,stat,comm" if threads else "pid,ppid,stat,comm", "--no-headers"]
    return parse_ps(subprocess.run(cmd, capture_output=True, text=True, check=True).stdout)


def compare(snap, p):
    k = {t["pid"]: t for t in snap["tasks"] if t["pid"] != 0 and not t.get("thread")}
    only_k = sorted(set(k) - set(p))
    only_p = sorted(set(p) - set(k))
    both = set(k) & set(p)
    bad_ppid = sorted(x for x in both if k[x]["ppid"] != p[x]["ppid"])
    bad_comm = sorted(x for x in both if not p[x]["comm"].startswith(k[x]["comm"][:15]))
    bad_state = sorted(x for x in both if k[x]["state"] != p[x]["stat"][0])
    print(f"  kernel: {len(k)} процесс (PID 0 ба thread-ээс бусад), ps: {len(p)} мөр, давхцсан: {len(both)}")
    print(f"  зөвхөн kernel-д: {len(only_k)} {[(x, k[x]['comm']) for x in only_k[:8]]}")
    print(f"  зөвхөн ps-д    : {len(only_p)} {[(x, p[x]['comm']) for x in only_p[:8]]}  (ps, cat зэрэг богино процесс)")
    print(f"  PPID таарсан   : {len(both) - len(bad_ppid)}/{len(both)} {bad_ppid[:10]}")
    print(f"  COMMAND таарсан: {len(both) - len(bad_comm)}/{len(both)} {bad_comm[:10]}")
    print(f"  STATE таарсан  : {len(both) - len(bad_state)}/{len(both)} {bad_state[:10]}  (хэмжилтийн агшин өөр)")
    match = len(both) / max(len(k), len(p), 1)
    print(f"  PID таарц      : {match:.1%}")
    return match, not bad_ppid


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--file"); ap.add_argument("--url")
    ap.add_argument("--proc", default="/proc/tasktree")
    ap.add_argument("--ps", action="store_true", help="одоо ажиллаж буй ps-тэй харьцуулах")
    ap.add_argument("--ps-file", help="хадгалсан `ps -o pid,ppid,stat,comm` гаралт")
    ap.add_argument("--min-match", type=float, default=0.90)
    args = ap.parse_args()
    snap = load(args)
    print(f"Эх үүсвэр: {snap.get('source')} snapshot #{snap.get('snapshot')} kernel {snap.get('kernel')}"
          f" count={snap.get('count')} truncated={snap.get('truncated', False)}")

    errs = check_dfs(snap)
    print(f"[{'OK' if not errs else 'FAIL'}] DFS pre-order: {len(snap['tasks'])} task, max_depth {snap.get('max_depth')}")
    for e in errs[:20]:
        print("   -", e)
    ok = not errs

    lin_ok, msg = check_linear(snap)
    print(f"[{'SKIP' if lin_ok is None else 'OK' if lin_ok else 'FAIL'}] LINEAR = DFS бүрэн байдал: {msg}")
    ok = ok and lin_ok is not False

    if args.ps_file or args.ps or (not args.file and not args.url):
        if args.ps_file:
            with open(args.ps_file, encoding="utf-8", errors="replace") as f:
                p = parse_ps(f.read())
        else:
            p = ps_table(False)
        m, ppid_ok = compare(snap, p)
        good = m >= args.min_match and ppid_ok
        print(f"[{'OK' if good else 'FAIL'}] ps таарц >= {args.min_match:.0%}, PPID зөрүүгүй")
        ok = ok and good
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
