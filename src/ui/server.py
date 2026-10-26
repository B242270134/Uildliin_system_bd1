#!/usr/bin/env python3
"""
server.py - Task graph UI-ийн жижиг HTTP сервер (зөвхөн Python стандарт сан).

Эх үүсвэр (--source):
  kernel     (анхдагч) /proc/tasktree-г уншина. tasktree.ko ачаалагдсан байх ёстой.
  userspace  /proc/<pid>/stat-аас ижил JSON-г user space-д бүтээнэ
             (харьцуулах тест, эсвэл module-гүй орчинд UI-г шалгах).
  file       --file замаар өгсөн хадгалсан snapshot (examples/*.json).

API:
  GET  /                 index.html
  GET  /api/tasks        одоогийн snapshot (уншиx бүрт шинэ)
  GET  /api/status       эх үүсвэр, module ачаалагдсан эсэх
  POST /api/spawn        туршилтын процессын мод үүсгэх  {depth, breadth, seconds, reap}
  POST /api/signal       туршилтын бүлэгт SIGSTOP/SIGCONT  {action: "stop"|"cont"}
  POST /api/kill         үүсгэсэн туршилтын модыг зогсоох
"""
import argparse
import json
import os
import signal
import subprocess
import sys
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
FORKTREE = ROOT / "tests" / "forktree.py"
PF_KTHREAD = 0x00200000

LIMITS = {"depth": (1, 5), "breadth": (1, 5), "seconds": (5, 600)}
spawned = []  # (Popen, reap) хосууд; reap=False бол zombie-г харуулахаар wait() хийхгүй


def reap_finished():
    """Дууссан хүүхдийг wait() хийж zombie-г цэвэрлэнэ (reap=True үед)."""
    for p, reap in spawned:
        if reap:
            p.poll()


class SourceError(Exception):
    def __init__(self, message, hint=""):
        super().__init__(message)
        self.hint = hint


# ---------------------------------------------------------------- sources
def read_kernel(proc_path):
    try:
        with open(proc_path, "r", encoding="utf-8", errors="replace") as f:
            raw = f.read()
    except FileNotFoundError:
        raise SourceError(
            f"{proc_path} олдсонгүй: tasktree kernel module ачаалагдаагүй байна.",
            "cd src/kernel && make && sudo insmod tasktree.ko  (эсвэл --source userspace)")
    except PermissionError:
        raise SourceError(f"{proc_path}-г унших эрхгүй.", "ls -l /proc/tasktree")
    try:
        return json.loads(raw)
    except json.JSONDecodeError as e:
        raise SourceError(f"Kernel-ээс ирсэн JSON буруу: {e}", "dmesg | tail")


def _read_stat(pid):
    with open(f"/proc/{pid}/stat", "rb") as f:
        data = f.read().decode("utf-8", "replace")
    l, r = data.index("("), data.rindex(")")
    comm = data[l + 1:r]
    rest = data[r + 2:].split()
    # rest[0]=state, [1]=ppid, [6]=flags, [15]=priority, [17]=num_threads, [19]=starttime
    return {
        "pid": pid, "comm": comm, "state": rest[0], "ppid": int(rest[1]),
        "kthread": bool(int(rest[6]) & PF_KTHREAD), "prio": int(rest[15]) + 100,
        "threads": int(rest[17]), "start_ticks": int(rest[19]),
    }


def read_userspace(show_threads=False):
    """Kernel module-ийн гаргадагтай ижил бүтэцтэй JSON-г /proc-оос бүтээнэ."""
    procs = {}
    for name in os.listdir("/proc"):
        if not name.isdigit():
            continue
        try:
            procs[int(name)] = _read_stat(int(name))
            procs[int(name)]["uid"] = os.stat(f"/proc/{name}").st_uid
        except (FileNotFoundError, ProcessLookupError, ValueError, IndexError):
            continue  # уншиж байхад процесс дууссан
    hz = os.sysconf("SC_CLK_TCK")
    children = {}
    for p in procs.values():
        parent = p["ppid"] if p["ppid"] in procs or p["ppid"] == 0 else 0
        children.setdefault(parent, []).append(p["pid"])
    for v in children.values():
        v.sort()

    tasks = []
    root = {"pid": 0, "comm": "swapper/0", "state": "R", "ppid": -1, "kthread": True,
            "prio": 120, "threads": 1, "start_ticks": 0, "uid": 0}

    def emit(p, depth, parent, is_thread=False):
        tasks.append({
            "order": len(tasks), "pid": p["pid"], "tgid": p.get("tgid", p["pid"]),
            "ppid": p["ppid"], "parent": parent, "depth": depth, "comm": p["comm"], "state": p["state"],
            "state_raw": None, "threads": p["threads"], "kthread": p["kthread"],
            "thread": is_thread, "uid": p["uid"], "prio": p["prio"],
            "start_ns": p["start_ticks"] * 1_000_000_000 // hz,
        })

    stack = [(root, 0, -1)]
    while stack:  # давталттай DFS (pre-order), хүүхдүүдийг PID өсөхөөр
        p, depth, ppid = stack.pop()
        emit(p, depth, ppid)
        if show_threads and p["pid"] != 0 and p["threads"] > 1:
            try:
                for tid in sorted(int(t) for t in os.listdir(f"/proc/{p['pid']}/task")):
                    if tid != p["pid"]:
                        t = _read_stat(f"{p['pid']}/task/{tid}")
                        t.update(pid=tid, tgid=p["pid"], uid=p["uid"])
                        emit(t, depth + 1, p["pid"], True)
            except (FileNotFoundError, ProcessLookupError, ValueError):
                pass
        for c in reversed(children.get(p["pid"], [])):
            stack.append((procs[c], depth + 1, p["pid"]))

    # for_each_process-ийн дараалал = процесс үүссэн дараалал (ойролцоогоор)
    linear = [p["pid"] for p in sorted(procs.values(), key=lambda p: (p["start_ticks"], p["pid"]))]
    return {
        "source": "userspace:/proc/<pid>/stat", "snapshot": None, "time": int(time.time()),
        "kernel": os.uname().release, "show_threads": show_threads, "cap": None,
        "truncated": False, "tasks": tasks, "linear": linear, "count": len(tasks),
        "linear_count": len(linear), "max_depth": max(t["depth"] for t in tasks),
    }


# ---------------------------------------------------------------- server
class Handler(BaseHTTPRequestHandler):
    cfg = None
    user_snap = 0

    def log_message(self, fmt, *args):
        if self.cfg.verbose:
            super().log_message(fmt, *args)

    def _send(self, code, body, ctype="application/json; charset=utf-8"):
        data = body if isinstance(body, bytes) else json.dumps(body, ensure_ascii=False).encode()
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Cache-Control", "no-store")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def _snapshot(self, show_threads):
        c = self.cfg
        if c.source == "kernel":
            return read_kernel(c.proc_path)
        if c.source == "userspace":
            snap = read_userspace(show_threads)
            Handler.user_snap += 1
            snap["snapshot"] = Handler.user_snap
            return snap
        try:
            with open(c.file, encoding="utf-8") as f:
                return json.load(f)
        except (OSError, json.JSONDecodeError) as e:
            raise SourceError(f"Snapshot файл уншиж чадсангүй: {e}", "--file examples/snapshot1_kernel.json")

    def do_GET(self):
        path, _, query = self.path.partition("?")
        if path in ("/", "/index.html"):
            return self._send(200, (HERE / "index.html").read_bytes(), "text/html; charset=utf-8")
        if path == "/api/tasks":
            reap_finished()
            show_threads = "threads=1" in query
            t0 = time.perf_counter()
            try:
                snap = self._snapshot(show_threads)
            except SourceError as e:
                return self._send(503, {"error": str(e), "hint": e.hint, "source": self.cfg.source})
            snap["read_ms"] = round((time.perf_counter() - t0) * 1000, 2)
            return self._send(200, snap)
        if path == "/api/status":
            loaded = os.path.exists(self.cfg.proc_path)
            return self._send(200, {
                "source": self.cfg.source, "proc_path": self.cfg.proc_path,
                "module_loaded": loaded, "file": self.cfg.file,
                "spawned": [p.pid for p, _ in spawned if p.returncode is None],
                "can_spawn": self.cfg.source != "file", "limits": LIMITS})
        return self._send(404, {"error": f"{path} олдсонгүй"})

    def do_POST(self):
        length = int(self.headers.get("Content-Length") or 0)
        try:
            body = json.loads(self.rfile.read(length) or b"{}")
        except json.JSONDecodeError:
            return self._send(400, {"error": "JSON буруу байна"})
        if self.path == "/api/spawn":
            if self.cfg.source == "file":
                return self._send(400, {"error": "file горимд шинэ процесс үүсгэх утгагүй (snapshot өөрчлөгдөхгүй)."})
            args = {}
            for key, (lo, hi) in LIMITS.items():
                val = body.get(key)
                if not isinstance(val, int) or isinstance(val, bool) or not lo <= val <= hi:
                    return self._send(400, {"error": f"'{key}' нь {lo}..{hi} хооронд бүхэл тоо байх ёстой (ирсэн: {val!r})."})
                args[key] = val
            reap = body.get("reap", True)
            threadfork = body.get("threadfork", False)
            if not isinstance(reap, bool) or not isinstance(threadfork, bool):
                return self._send(400, {"error": "'reap', 'threadfork' нь true/false байна."})
            total = sum(args["breadth"] ** d for d in range(1, args["depth"] + 1))
            if total > 200:
                return self._send(400, {"error": f"Нийт {total} процесс үүснэ; 200-аас ихгүй байхаар depth/breadth-ийг багасгана уу."})
            p = subprocess.Popen([sys.executable, str(FORKTREE), str(args["depth"]),
                                  str(args["breadth"]), str(args["seconds"])]
                                 + (["thread"] if threadfork else []),
                                 start_new_session=True)
            spawned.append((p, reap))
            return self._send(200, {"ok": True, "root_pid": p.pid, "processes": total + 1,
                                    "reap": reap, "threadfork": threadfork, **args})
        if self.path == "/api/signal":
            # Зөвхөн энэ серверийн үүсгэсэн туршилтын бүлэгт дохио өгнө (аюулгүй байдал)
            sig = {"stop": signal.SIGSTOP, "cont": signal.SIGCONT}.get(body.get("action"))
            if sig is None:
                return self._send(400, {"error": "'action' нь \"stop\" эсвэл \"cont\" байна."})
            alive = [p for p, reap in spawned if (p.poll() is None if reap else p.returncode is None)]
            if not alive:
                return self._send(400, {"error": "Ажиллаж буй туршилтын процесс алга. Эхлээд fork() мод үүсгэнэ үү."})
            done = []
            for p in alive:
                try:
                    os.killpg(p.pid, sig)
                    done.append(p.pid)
                except ProcessLookupError:
                    pass
            return self._send(200, {"ok": True, "action": body["action"], "groups": done})
        if self.path == "/api/kill":
            killed = []
            for p, _ in spawned:
                try:
                    os.killpg(p.pid, signal.SIGTERM)
                    os.killpg(p.pid, signal.SIGCONT)  # SIGSTOP-оор зогссон бол SIGTERM-ийг хүлээн авахын тулд
                    killed.append(p.pid)
                except ProcessLookupError:
                    pass
                try:
                    p.wait(timeout=3)  # zombie үлдээхгүй
                except subprocess.TimeoutExpired:
                    os.killpg(p.pid, signal.SIGKILL)
                    p.wait()
            spawned.clear()
            return self._send(200, {"ok": True, "killed_groups": killed})
        return self._send(404, {"error": f"{self.path} олдсонгүй"})


def main():
    ap = argparse.ArgumentParser(description="Task graph UI server")
    ap.add_argument("--source", choices=["kernel", "userspace", "file"], default="kernel")
    ap.add_argument("--file", help="--source file үед snapshot JSON файл")
    ap.add_argument("--proc-path", default="/proc/tasktree")
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=8000)
    ap.add_argument("--verbose", action="store_true")
    cfg = ap.parse_args()
    if cfg.source == "file" and not cfg.file:
        ap.error("--source file үед --file заавал өгнө")
    if not 1 <= cfg.port <= 65535:
        ap.error("--port 1..65535")
    if cfg.source == "kernel" and not os.path.exists(cfg.proc_path):
        print(f"[!] {cfg.proc_path} алга: эхлээд `sudo insmod src/kernel/tasktree.ko` хийнэ үү. "
              "Сервер ажиллана, UI алдааг харуулна.", file=sys.stderr)
    Handler.cfg = cfg
    srv = ThreadingHTTPServer((cfg.host, cfg.port), Handler)
    print(f"Task graph UI: http://{cfg.host}:{cfg.port}  (source={cfg.source})")
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        for p, _ in spawned:
            try:
                os.killpg(p.pid, signal.SIGTERM)
            except ProcessLookupError:
                pass


if __name__ == "__main__":
    main()
