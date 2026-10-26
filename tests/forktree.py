#!/usr/bin/env python3
"""
forktree.py DEPTH BREADTH SECONDS [thread]

fork()-оор DEPTH түвшин, түвшин бүрт BREADTH хүүхэдтэй процессын мод үүсгэнэ.
Процесс бүрийн нэрийг (comm) prctl(PR_SET_NAME)-ээр "bd1-L<түвшин>-<дугаар>"
болгоно, ингэснээр task graph дээр ялгаж харж болно. SECONDS дараа навчнууд
гарч, эцэг бүр wait()-ээр хүүхдээ цуглуулна (zombie үлдэхгүй).

Жишээ:  python3 tests/forktree.py 2 3 60   ->  1 + 3 + 9 = 13 процесс

"thread" горим: үндэс процесс эхний түвшний хүүхдүүдээ WORKER THREAD-ээс
fork() хийнэ. Тэдгээрийн real_parent нь worker thread тул kernel тэднийг
thread-ийн children жагсаалтад хадгална (leader-ийнх биш) - task graph
дээр зөв харагдаж байгаа эсэхийг шалгах тест.
"""
import ctypes
import os
import signal
import sys
import threading
import time
import warnings

warnings.filterwarnings("ignore", category=DeprecationWarning)  # олон thread-тэй үед fork

PR_SET_NAME = 15
KIDS = []  # энэ процессын хүүхдүүд


def on_term(signum, frame):
    """SIGTERM ирэхэд хүүхдүүдээ зогсоож, wait()-ээр цуглуулаад гарна.
    Ингэснээр орфан/zombie процесс үлдэхгүй."""
    for k in KIDS:
        try:
            os.kill(k, signal.SIGTERM)
        except ProcessLookupError:
            pass
    for k in KIDS:
        try:
            os.waitpid(k, 0)
        except ChildProcessError:
            pass
    os._exit(0)


def set_name(name):
    try:
        libc = ctypes.CDLL(None, use_errno=True)
        libc.prctl(PR_SET_NAME, ctypes.c_char_p(name.encode()[:15]), 0, 0, 0)
    except OSError:
        pass


def spawn_children(level, depth, breadth, seconds):
    for i in range(breadth):
        pid = os.fork()
        if pid == 0:
            node(level + 1, i, depth, breadth, seconds)
            os._exit(0)
        KIDS.append(pid)


def wait_kids():
    for k in list(KIDS):
        try:
            os.waitpid(k, 0)
        except ChildProcessError:
            pass


def node(level, idx, depth, breadth, seconds, from_thread=False):
    set_name(f"bd1-L{level}-{idx}")
    KIDS.clear()  # fork хийсэн хүүхэд эцгийнхээ жагсаалтыг өвлөхгүй
    signal.signal(signal.SIGTERM, on_term)
    if level >= depth:
        time.sleep(seconds)          # навч
        return
    if from_thread:
        # Worker thread хүүхдүүдээ fork хийгээд ӨӨРӨӨ хүлээнэ. Thread амьд байх
        # хугацаанд хүүхдүүд нь thread-ийн children жагсаалтад үлдэнэ (thread
        # дуусвал kernel тэднийг group-ийн өөр thread руу шилжүүлдэг).
        def worker():
            set_name("bd1-worker")   # энэ thread-ийн нэр
            spawn_children(level, depth, breadth, seconds)
            wait_kids()
        th = threading.Thread(target=worker)
        th.start()
        th.join()
    else:
        spawn_children(level, depth, breadth, seconds)
        wait_kids()


def main():
    try:
        depth, breadth, seconds = (int(a) for a in sys.argv[1:4])
    except ValueError:
        sys.exit("usage: forktree.py DEPTH BREADTH SECONDS [thread] (бүхэл тоо)")
    from_thread = len(sys.argv) > 4 and sys.argv[4] == "thread"
    if not (1 <= depth <= 5 and 1 <= breadth <= 5 and 1 <= seconds <= 3600):
        sys.exit("depth 1..5, breadth 1..5, seconds 1..3600")
    node(0, 0, depth, breadth, seconds, from_thread)


if __name__ == "__main__":
    main()
