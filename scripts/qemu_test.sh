#!/usr/bin/env bash
# qemu_test.sh - Module-уудыг БОДИТ Linux kernel дээр QEMU-д автоматаар турших.
#
# Таны host компьютерийн kernel-д нөлөөлөхгүй: тусдаа виртуал машин асааж,
# busybox initramfs дотор insmod / rmmod / туршилтын процессуудыг ажиллуулна.
#
# Шаардлага (Ubuntu):
#   sudo apt install -y qemu-system-x86 busybox-static cpio gcc python3
#   make -C src/kernel                     # module-ууд build хийгдсэн байх
# Хэрэглээ:
#   bash scripts/qemu_test.sh [KERNEL_IMAGE] [OLD_TASKTREE_KO]
#     KERNEL_IMAGE   - анхдагч /boot/vmlinuz-$(uname -r) (module-ийн header-тэй ижил хувилбар!)
#                      Унших эрхгүй бол: sudo cp /boot/vmlinuz-$(uname -r) /tmp/vmlinuz && sudo chmod a+r /tmp/vmlinuz
#     OLD_TASKTREE_KO - (заавал биш) өмнөх хувилбарын tasktree.ko-г мөн туршиж харьцуулна
# Гаралт: examples/qemu/ (serial.log, JSON snapshot, ps гаралт, results.txt)
set -euo pipefail
cd "$(dirname "$0")/.."
ROOT=$PWD
KERNEL=${1:-/boot/vmlinuz-$(uname -r)}
OLD_KO=${2:-}
OUT=$ROOT/examples/qemu
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

for c in qemu-system-x86_64 busybox cpio gcc python3; do
  command -v $c >/dev/null || { echo "[!] $c алга: sudo apt install -y qemu-system-x86 busybox-static cpio gcc python3"; exit 1; }
done
[ -r "$KERNEL" ] || { echo "[!] kernel image уншигдахгүй: $KERNEL"; exit 1; }
for m in tasklist_linear tasklist_dfs tasktree; do
  [ -f src/kernel/$m.ko ] || { echo "[!] src/kernel/$m.ko алга: make -C src/kernel"; exit 1; }
done

echo "== initramfs бэлдэх"
R=$WORK/rootfs
mkdir -p $R/{bin,proc,sys,dev,tmp,mod}
cp "$(command -v busybox)" $R/bin/busybox
for a in sh mount insmod rmmod lsmod dmesg cat ps sleep echo poweroff grep wc head tail sed kill ls seq; do
  ln -sf busybox $R/bin/$a
done
cp src/kernel/*.ko $R/mod/
[ -n "$OLD_KO" ] && cp "$OLD_KO" $R/mod/old_tasktree.ko
gcc -static -O2 -pthread tests/bd1_threads.c -o $R/bin/bd1-threads
gcc -static -O2 -pthread tests/thread_fork.c -o $R/bin/bd1-thrfork

cat > $R/init <<'EOF'
#!/bin/sh
mount -t proc proc /proc; mount -t sysfs sys /sys; mount -t devtmpfs dev /dev
dmesg -n 1
res(){ echo "RESULT $1 $2 $3"; }
echo "=== KERNEL $(cat /proc/sys/kernel/osrelease)"

# --- Туршилтын ачаалал: thread, thread-ээс fork, процессын мод
/bin/bd1-threads &
/bin/bd1-thrfork 900 &
sh -c 'sh -c "sleep 900 & sleep 900 & wait" & sleep 900 & wait' &
TREE=$!
sleep 1
echo "=== TREEPID $TREE"

# --- K1-K3: буруу параметр
insmod /mod/tasktree.ko show_threads=abc 2>/dev/null && res K1 FAIL "show_threads=abc accepted" || res K1 PASS "show_threads=abc rejected"
insmod /mod/tasktree.ko max_tasks=1 2>/dev/null && res K2 FAIL "max_tasks=1 accepted" || res K2 PASS "max_tasks=1 rejected"
insmod /mod/tasktree.ko max_tasks=99999 2>/dev/null && res K3 FAIL "max_tasks=99999 accepted" || res K3 PASS "max_tasks=99999 rejected"
dmesg | grep "tasktree:" | tail -3

# --- K4: Part I
dmesg -c > /dev/null
insmod /mod/tasklist_linear.ko && rmmod tasklist_linear
echo "=== DMESG_LINEAR_BEGIN"; dmesg | grep "tasklist_linear:"; echo "=== DMESG_LINEAR_END"
N=$(dmesg | grep -c "tasklist_linear: .* [0-9][0-9]* *[0-9-][0-9]* *[A-Z] ")
[ "$N" -gt 0 ] && res K4 PASS "Part I listed $N tasks" || res K4 FAIL "Part I empty"

# --- K5: Part II (thread-ээс fork хийсэн хүүхэд DFS-д харагдах ёстой)
dmesg -c > /dev/null
insmod /mod/tasklist_dfs.ko && rmmod tasklist_dfs
echo "=== DMESG_DFS_BEGIN"; dmesg | grep "tasklist_dfs:"; echo "=== DMESG_DFS_END"
C=$(dmesg | grep -c "bd1-thrchild")
[ "$C" = 2 ] && res K5 PASS "Part II shows 2 thread-forked children" || res K5 FAIL "Part II thread-forked children=$C"

# --- K6-K7: tasktree ачаалах, давхар ачаалах
insmod /mod/tasktree.ko show_threads=1 && res K6 PASS "tasktree loaded" || res K6 FAIL "tasktree load"
insmod /mod/tasktree.ko 2>/dev/null && res K7 FAIL "double insmod accepted" || res K7 PASS "double insmod rejected (File exists)"

echo "=== SNAP workload BEGIN"; cat /proc/tasktree; echo "=== SNAP workload END"
echo "=== PS workload BEGIN"; ps -o pid,ppid,stat,comm; echo "=== PS workload END"

# --- K8: 30 удаа дараалан унших
i=0; while [ $i -lt 30 ]; do cat /proc/tasktree > /dev/null || break; i=$((i+1)); done
[ $i = 30 ] && res K8 PASS "30 consecutive reads" || res K8 FAIL "read failed at $i"

# --- SIGSTOP -> T төлөв
kill -STOP $TREE; sleep 1
echo "=== SNAP stopped BEGIN"; cat /proc/tasktree; echo "=== SNAP stopped END"
kill -CONT $TREE

# --- max_tasks: ажиллаж байхад өөрчлөх
echo 5 > /sys/module/tasktree/parameters/max_tasks 2>/dev/null && res K9 FAIL "runtime max_tasks=5 accepted" || res K9 PASS "runtime max_tasks=5 rejected"
echo 16 > /sys/module/tasktree/parameters/max_tasks
echo "=== SNAP capped BEGIN"; cat /proc/tasktree; echo "=== SNAP capped END"
echo 4096 > /sys/module/tasktree/parameters/max_tasks

# --- Хуучин хувилбартай харьцуулах (заавал биш)
if [ -f /mod/old_tasktree.ko ]; then
  rmmod tasktree; insmod /mod/old_tasktree.ko show_threads=1
  echo "=== SNAP old BEGIN"; cat /proc/tasktree; echo "=== SNAP old END"
  rmmod tasktree; insmod /mod/tasktree.ko show_threads=1
fi

# --- Ачааллыг зогсоох -> алга болох
kill $TREE 2>/dev/null; kill $(ps -o pid,comm | grep -E "bd1-|sleep" | sed 's/^ *//' | cut -d' ' -f1) 2>/dev/null
sleep 1
echo "=== SNAP after BEGIN"; cat /proc/tasktree; echo "=== SNAP after END"

# --- K10: буулгах
rmmod tasktree
[ -e /proc/tasktree ] && res K10 FAIL "/proc/tasktree remains" || res K10 PASS "rmmod removed /proc/tasktree"
dmesg | grep -iE "BUG|Oops|WARNING|lockup" && res K11 FAIL "kernel warnings" || res K11 PASS "no BUG/Oops/WARNING"
echo "=== DONE"
poweroff -f
EOF
chmod +x $R/init
ln -sf busybox $R/bin/cut
(cd $R && find . | cpio -o -H newc 2>/dev/null | gzip) > $WORK/initramfs.gz

echo "== QEMU асааж байна (KVM байхгүй бол 1-2 минут)"
ACCEL=""; [ -w /dev/kvm ] && ACCEL="-enable-kvm"
mkdir -p "$OUT"
timeout 600 qemu-system-x86_64 $ACCEL -m 1024 -smp 2 -nographic -no-reboot \
  -kernel "$KERNEL" -initrd $WORK/initramfs.gz \
  -append "console=ttyS0 quiet panic=-1" | tr -d '\r' > "$OUT/serial.log" || true

grep -q "=== DONE" "$OUT/serial.log" || { echo "[!] QEMU бүрэн ажиллаж дуусаагүй (лог: $OUT/serial.log). Дахин ажиллуулна уу."; exit 2; }

echo "== Гаралт задлах, шалгах"
python3 - "$OUT" <<'PY'
import json, re, sys, os
out = sys.argv[1]
log = open(os.path.join(out, "serial.log"), encoding="utf-8", errors="replace").read()
def block(name):
    m = re.search(rf"=== {name} BEGIN\n(.*?)\n=== {name} END", log, re.S)
    return m.group(1) if m else None
def section(b, e):
    m = re.search(rf"=== {b}\n(.*?)\n=== {e}", log, re.S)
    return m.group(1) if m else ""
for n in ["workload", "stopped", "capped", "old", "after"]:
    b = block(f"SNAP {n}")
    if b:
        open(os.path.join(out, f"snap_{n}.json"), "w").write(b + "\n")
open(os.path.join(out, "ps_workload.txt"), "w").write((block("PS workload") or "") + "\n")
open(os.path.join(out, "dmesg_part1.txt"), "w").write(section("DMESG_LINEAR_BEGIN", "DMESG_LINEAR_END") + "\n")
open(os.path.join(out, "dmesg_part2.txt"), "w").write(section("DMESG_DFS_BEGIN", "DMESG_DFS_END") + "\n")
results = re.findall(r"^RESULT (\S+) (\S+) (.*)$", log, re.M)
tree = int(re.search(r"=== TREEPID (\d+)", log).group(1))

def load(n):
    p = os.path.join(out, f"snap_{n}.json")
    return json.load(open(p)) if os.path.exists(p) else None
def lin_complete(s):
    dfs = {t["pid"] for t in s["tasks"] if not t.get("thread") and t["pid"] != 0}
    return sorted(set(s["linear"]) - dfs) if "linear" in s else None
def add(code, ok, desc):
    results.append((code, "PASS" if ok else "FAIL", desc))

w = load("workload")
if w:
    by = {t["pid"]: t for t in w["tasks"] if not t.get("thread")}
    missing = lin_complete(w)
    add("H1", missing == [], f"workload snapshot: {w['count']} tasks, LINEAR {len(w.get('linear', []))}, missing in DFS {missing}")
    fork = [t for t in by.values() if t["comm"] == "bd1-thrfork"]
    kids = [t for t in by.values() if t["comm"] == "bd1-thrchild"]
    ok = len(fork) == 1 and len(kids) == 2 and all(k["ppid"] == fork[0]["pid"] == k["parent"] for k in kids)
    add("H2", ok, f"thread-forked children {[ (k['pid'], k['ppid'], k['parent']) for k in kids]} of bd1-thrfork {fork[0]['pid'] if fork else None}")
    thr = [t for t in w["tasks"] if t.get("thread") and t["comm"] == "bd1-threads"]
    lead = [t for t in by.values() if t["comm"] == "bd1-threads"]
    add("H3", len(thr) == 3 and lead and all(t["tgid"] == lead[0]["pid"] for t in thr), f"show_threads: {len(thr)} threads of bd1-threads")
    rows = {}
    for line in open(os.path.join(out, "ps_workload.txt")):
        p = line.split(None, 3)
        if len(p) == 4 and p[0].isdigit():
            rows[int(p[0])] = (int(p[1]), p[2], p[3].strip())
    both = [p for p in by if p in rows and p != 0]
    bad = [p for p in both if by[p]["ppid"] != rows[p][0]]
    add("H4", len(both) >= 0.9 * len(rows) and not bad, f"ps vs kernel: common {len(both)}/{len(rows)}, PPID mismatches {bad}")
s = load("stopped")
if s:
    t = next((t for t in s["tasks"] if t["pid"] == tree), None)
    add("H5", t is not None and t["state"] == "T", f"SIGSTOP tree root {tree} state={t and t['state']}")
c = load("capped")
if c:
    add("H6", c["count"] == 16 and c["truncated"] is True, f"max_tasks=16: count={c['count']} truncated={c['truncated']}")
a = load("after")
if a:
    left = [t["comm"] for t in a["tasks"] if t["comm"].startswith("bd1-")]
    add("H7", not left, f"after kill: bd1-* left {left}")
o = load("old")
if o:
    # Хуучин хувилбар зөвхөн leader-ийн children-ийг гүйдэг: thread-ээс fork хийсэн
    # хүүхдүүд алга байх ёстой (алдааг давтан үзүүлэх нотолгоо).
    new_kids = sorted(t["pid"] for t in w["tasks"] if t["comm"] == "bd1-thrchild") if w else []
    old_kids = sorted(t["pid"] for t in o["tasks"] if t["comm"] == "bd1-thrchild")
    add("H8", bool(new_kids) and not old_kids,
        f"OLD module (leader-only DFS): bd1-thrchild {old_kids} vs NEW {new_kids} -> bug reproduced & fixed")
with open(os.path.join(out, "results.txt"), "w") as f:
    for r in results:
        line = f"{r[0]:4} {r[1]:4} {r[2]}"
        print(line); f.write(line + "\n")
    npass = sum(r[1] == "PASS" for r in results)
    summary = f"Нийт: {npass}/{len(results)} PASS"
    print(summary); f.write(summary + "\n")
sys.exit(0 if npass == len(results) else 1)
PY
