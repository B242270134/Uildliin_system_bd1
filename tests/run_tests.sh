#!/usr/bin/env bash
# run_tests.sh - module ачаалахгүйгээр шалгаж болох автомат тестүүд.
#   bash tests/run_tests.sh
set -u
cd "$(dirname "$0")/.."
PASS=0; FAIL=0
ok(){ echo "  [PASS] $*"; PASS=$((PASS+1)); }
no(){ echo "  [FAIL] $*"; FAIL=$((FAIL+1)); }
expect(){ local want=$1; shift; "$@" >/dev/null 2>&1; local got=$?; [ "$got" = "$want" ] && ok "$* (exit $got)" || no "$* (exit $got, хүлээсэн $want)"; }

echo "T1. Хадгалсан kernel snapshot-уудын DFS дараалал"
for f in examples/snapshot1_kernel.json examples/snapshot2_kernel.json examples/snapshot_threads_kernel.json; do
  expect 0 python3 tests/compare_ps.py --file "$f"
done

echo "T1b. QEMU (Linux 6.8.0-31) дээрх шинэ snapshot: DFS + LINEAR бүрэн байдал + ps-тэй тулгалт"
expect 0 python3 tests/compare_ps.py --file examples/qemu/snap_workload.json --ps-file examples/qemu/ps_workload.txt
expect 0 python3 tests/compare_ps.py --file examples/qemu/snap_stopped.json
python3 - <<'PY' && ok "max_tasks=16 snapshot: count=16, truncated=true" || no "capped snapshot"
import json; c=json.load(open("examples/qemu/snap_capped.json")); assert c["count"]==16 and c["truncated"] is True
PY
python3 - <<'PY' && ok "thread-ээс fork хийсэн 2 хүүхэд шинэ модульд бий, хуучинд алга" || no "thread-fork нотолгоо"
import json
n=json.load(open("examples/qemu/snap_workload.json")); o=json.load(open("examples/qemu/snap_old.json"))
kn=[t for t in n["tasks"] if t["comm"]=="bd1-thrchild"]; ko=[t for t in o["tasks"] if t["comm"]=="bd1-thrchild"]
f=[t for t in n["tasks"] if t["comm"]=="bd1-thrfork"][0]
assert len(kn)==2 and not ko and all(k["ppid"]==f["pid"]==k["parent"] for k in kn)
PY

echo "T2. Санаатай эвдсэн DFS дарааллыг илрүүлэх (сөрөг тест)"
expect 1 python3 tests/compare_ps.py --file tests/bad_dfs_order.json

echo "T3. Хоёр snapshot-ын ялгаа (дахин уншихад шинэчлэгдсэн эсэх)"
python3 - <<'EOF' && ok "snapshot #1 -> #2: шинэ task илэрсэн" || no "snapshot ялгаа"
import json
a=json.load(open("examples/snapshot1_kernel.json")); b=json.load(open("examples/snapshot2_kernel.json"))
ka={(t["pid"],t["start_ns"]) for t in a["tasks"]}; kb={(t["pid"],t["start_ns"]) for t in b["tasks"]}
print("  added:",sorted(kb-ka),"gone:",sorted(ka-kb))
assert b["snapshot"]==a["snapshot"]+1 and kb-ka
EOF

echo "T4. Сервер API (userspace горим, порт 8199)"
python3 src/ui/server.py --source userspace --port 8199 >/dev/null 2>&1 & SRV=$!
sleep 1
code(){ curl -s -o /dev/null -w '%{http_code}' "$@"; }
[ "$(code http://127.0.0.1:8199/api/tasks)" = 200 ] && ok "GET /api/tasks 200" || no "GET /api/tasks"
[ "$(code -XPOST http://127.0.0.1:8199/api/spawn -d '{"depth":0,"breadth":2,"seconds":10}')" = 400 ] && ok "depth=0 -> 400" || no "depth=0"
[ "$(code -XPOST http://127.0.0.1:8199/api/spawn -d '{"depth":5,"breadth":5,"seconds":10}')" = 400 ] && ok "3905 процесс -> 400" || no "too many"
[ "$(code -XPOST http://127.0.0.1:8199/api/spawn -d 'abc')" = 400 ] && ok "JSON биш -> 400" || no "bad json"
[ "$(code -XPOST http://127.0.0.1:8199/api/spawn -d '{"depth":1,"breadth":2,"seconds":5}')" = 200 ] && ok "spawn 1x2 -> 200" || no "spawn"
sleep 1
N=$(curl -s http://127.0.0.1:8199/api/tasks | python3 -c "import json,sys;print(sum(t['comm'].startswith('bd1-') for t in json.load(sys.stdin)['tasks']))")
[ "$N" = 3 ] && ok "граф дээр bd1-* 3 процесс гарсан" || no "bd1 тоо=$N (3 хүлээсэн)"
curl -s -XPOST http://127.0.0.1:8199/api/kill >/dev/null; sleep 1
N=$(curl -s http://127.0.0.1:8199/api/tasks | python3 -c "import json,sys;print(sum(t['comm'].startswith('bd1-') for t in json.load(sys.stdin)['tasks']))")
[ "$N" = 0 ] && ok "kill-ийн дараа bd1-* алга (zombie үлдээгүй)" || no "kill дараа bd1=$N"
[ "$(code -XPOST http://127.0.0.1:8199/api/signal -d '{"action":"stop"}')" = 400 ] && ok "туршилтын процессгүй SIGSTOP -> 400" || no "signal no-proc"
[ "$(code -XPOST http://127.0.0.1:8199/api/signal -d '{"action":"kill9"}')" = 400 ] && ok "буруу action -> 400" || no "signal bad action"
curl -s -XPOST http://127.0.0.1:8199/api/spawn -d '{"depth":1,"breadth":2,"seconds":20,"threadfork":true}' >/dev/null; sleep 1
R=$(curl -s "http://127.0.0.1:8199/api/tasks?threads=1" | python3 -c "
import json,sys; d=json.load(sys.stdin); t=d['tasks']
root=[x for x in t if x['comm']=='bd1-L0-0' and not x['thread']][0]
kids=[x for x in t if x['comm'].startswith('bd1-L1') and x['parent']==root['pid'] and x['ppid']==root['pid']]
w=[x for x in t if x['thread'] and x['tgid']==root['pid']]
print(len(kids), len(w))")
[ "$R" = "2 1" ] && ok "worker thread-ээс fork: 2 хүүхэд root-д, 1 thread" || no "threadfork: $R"
[ "$(code -XPOST http://127.0.0.1:8199/api/signal -d '{"action":"stop"}')" = 200 ] && ok "SIGSTOP -> 200" || no "SIGSTOP"
sleep 0.5
S=$(curl -s http://127.0.0.1:8199/api/tasks | python3 -c "import json,sys;print(''.join(sorted({t['state'] for t in json.load(sys.stdin)['tasks'] if t['comm'].startswith('bd1-')})))")
[ "$S" = "T" ] && ok "SIGSTOP дараа бүх bd1-* төлөв T" || no "stopped states=$S"
curl -s -XPOST http://127.0.0.1:8199/api/signal -d '{"action":"cont"}' >/dev/null
curl -s -XPOST http://127.0.0.1:8199/api/kill >/dev/null; sleep 1
N=$(curl -s http://127.0.0.1:8199/api/tasks | python3 -c "import json,sys;print(sum(t['comm'].startswith('bd1-') for t in json.load(sys.stdin)['tasks']))")
[ "$N" = 0 ] && ok "threadfork модыг зогсооход бүгд цэвэрлэгдсэн" || no "threadfork kill left=$N"
kill $SRV

echo "T5. Module ачаалаагүй үеийн алдааны мэдээлэл (kernel горим)"
python3 src/ui/server.py --source kernel --proc-path /proc/does_not_exist --port 8198 >/dev/null 2>&1 & SRV=$!
sleep 1
[ "$(code http://127.0.0.1:8198/api/tasks)" = 503 ] && ok "/proc/tasktree алга -> 503 + hint" || no "503"
kill $SRV

echo; echo "Нийт: $PASS PASS, $FAIL FAIL"
[ $FAIL = 0 ]
