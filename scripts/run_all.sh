#!/usr/bin/env bash
# run_all.sh - Build -> Part I -> Part II -> tasktree -> UI-г дараалан ажиллуулна.
# Ubuntu 22.04/24.04 VM дээр:  bash scripts/run_all.sh
set -euo pipefail
cd "$(dirname "$0")/.."
K=src/kernel
LOG=examples/my_run_$(date +%Y%m%d_%H%M%S)
mkdir -p "$LOG"

step(){ printf '\n\033[1;34m== %s ==\033[0m\n' "$*"; }

step "0. Орчин шалгах"
uname -r
[ -d "/lib/modules/$(uname -r)/build" ] || { echo "Kernel headers алга: sudo apt install linux-headers-$(uname -r)"; exit 1; }

step "1. Build"
make -C "$K" clean >/dev/null
make -C "$K"

# Өмнө ачаалагдсан бол буулгана (дахин ажиллуулахад алдаа гаргахгүй)
for m in tasklist_linear tasklist_dfs tasktree; do
  lsmod | grep -q "^$m " && sudo rmmod "$m" || true
done

step "2. Part I - шугаман жагсаалт (for_each_process)"
sudo dmesg -C
sudo insmod "$K/tasklist_linear.ko"
sudo dmesg | tee "$LOG/part1_dmesg.txt" | tail -n 15
ps -el > "$LOG/part1_ps_el.txt"
echo "dmesg мөр: $(grep -c 'tasklist_linear: .* [0-9]' "$LOG/part1_dmesg.txt")  |  ps -el мөр: $(($(wc -l < "$LOG/part1_ps_el.txt")-1))"
sudo rmmod tasklist_linear

step "3. Part II - DFS мод (init_task -> children/sibling)"
sudo dmesg -C
sudo insmod "$K/tasklist_dfs.ko"
sudo dmesg | tee "$LOG/part2_dmesg.txt" | head -n 25
ps -eLf > "$LOG/part2_ps_eLf.txt"
sudo rmmod tasklist_dfs

step "4. tasktree -> /proc/tasktree (JSON)"
sudo insmod "$K/tasktree.ko"
cat /proc/tasktree > "$LOG/snapshot_a.json"
sleep 1
cat /proc/tasktree > "$LOG/snapshot_b.json"
python3 tests/compare_ps.py | tee "$LOG/compare_ps.txt"

step "5. UI: http://127.0.0.1:8000  (Ctrl+C зогсооно)"
echo "Гаралтууд: $LOG"
python3 src/ui/server.py --source kernel --port 8000
