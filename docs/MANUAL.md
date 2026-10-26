# Manual — Task Graph (Linux Kernel Module for Listing Tasks)

## 8.1 Хэрэглэгчийн заавар

**Зорилго.** Kernel module-оор Linux-ийн бүх task-ыг жагсааж, эцэг–хүүхдийн модыг граф хэлбэрээр
харуулах; дахин уншихад бодит жагсаалт шинэчлэгдэхийг харах.

**Дэмжих орчин.** Ubuntu 22.04/24.04 (kernel 5.15–6.8 шалгасан хэлбэрээр бичсэн, 6.8.0-31 дээр бодитоор туршсан),
x86_64, VirtualBox/VMware/UTM VM. Secure Boot унтраалттай (эсвэл module-ийг гарын үсэглэсэн) байх.
Хөтөч: Chrome/Firefox/Edge-ийн сүүлийн хувилбар.

**Нээх алхам.** `sudo insmod src/kernel/tasktree.ko` → `python3 src/ui/server.py` → хөтчөөр `http://127.0.0.1:8000`.
VM-ийн гаднаас нээх бол `--host 0.0.0.0` (зөвхөн найдвартай сүлжээнд).

**Оролт ба тохиргоо**

| Удирдлага | Утга | Хүчинтэй муж |
| --- | --- | --- |
| Refresh | `/proc/tasktree`-г шинээр уншина (snapshot # нэмэгдэнэ) | — |
| Автомат refresh / Интервал | Тогтмол хугацаанд уншина | 1–60 сек |
| Thread-уудыг харуулах | userspace горимд thread нэмнэ; kernel горимд `show_threads` параметрээр | — |
| Жагсаалтын дараалал | DFS (Part II) эсвэл LINEAR — kernel-ийн `for_each_process` дараалал (Part I) | — |
| Түүх | Сүүлийн 20 snapshot-оос сонгож эргэн харах | — |
| Root PID | Энэ PID-ийн дэд модыг харуулна | snapshot-д байгаа PID |
| Хамгийн их гүн | Түүнээс доош хураана | 1–64 |
| kthreadd-ийн хүүхдийг хураах | ~100 kernel thread-ийг нэг зангилаанд нуух | — |
| Kernel thread-ийг нуух | PF_KTHREAD тугтайг бүрэн нуух | — |
| Хайх | Нэр эсвэл PID-ээр шар цагирагаар тодруулах | — |
| DFS алхамчлах | Гулсуур / Тоглуулах: харагдаж буй модыг DFS дарааллаар нэг нэгээр нь гэрэлтүүлнэ | 0..N |
| fork() мод үүсгэх | `forktree.py`-г ажиллуулж гүн×салбартай процессын мод үүсгэнэ | гүн 1–5, салбар 1–5, ≤200 процесс, 5–600 сек |
| wait() хийхгүй | Үндэс процесс дуусахад zombie (Z) болж үлдэхийг харуулна | — |
| Worker thread-ээс fork() | Эхний түвшний хүүхдүүдийг тусдаа thread-ээс fork хийнэ — kernel тэднийг thread-ийн `children` жагсаалтад хадгалдгийг шалгах | — |
| SIGSTOP / SIGCONT | Зөвхөн үүсгэсэн туршилтын бүлэгт дохио: төлөв T болж, дахин S болно | — |
| Зогсоох | Үүсгэсэн модыг SIGTERM-ээр зогсоож wait() хийнэ | — |
| Reset | Харагдацын бүх тохиргоог анхны байдалд | — |

**Ажиллуулах / зогсоох / reset.** Сервер: `Ctrl+C`. Module: `sudo rmmod tasktree`. UI: Reset товч.

**Дүрслэлийн тэмдэг, өнгө, төлөв**

| Тэмдэг | Утга |
| --- | --- |
| Ногоон / цэнхэр / улбар шар / саарал / улаан / ягаан өнгө | R running·ready / S sleeping / D disk sleep / I idle kthread / Z zombie / T stopped |
| Дугуй ● | user процесс · Дөрвөлжин ■ kernel thread · Жижиг дугуй + тасархай ирмэг — thread |
| Ногоон цагираг | өмнөх snapshot-оос хойш шинээр үүссэн task |
| Улбар шар цагираг | төлөв өөрчлөгдсөн (ж: S→Z) |
| Шар гадна цагираг | хайлтын илэрц эсвэл DFS-ийн одоогийн алхам |
| `+N` | хураагдсан дэд модны task-ын тоо · `⧉N` thread-ийн тоо |

**Жишээ 1 — fork() мод.** Гүн 2, салбар 3, 60 сек → «fork() мод үүсгэх». 13 шинэ зангилаа ногоон цагирагтай гарна,
«Өөрчлөлт» хэсэгт `+ PID bd1-L…` 13 мөр. «Зогсоох» → Refresh: 13 мөр `−` болж граф дээрээс алга.

**Жишээ 2 — zombie.** Гүн 1, салбар 2, 5 сек, «wait() хийхгүй»-г чагтлаад үүсгэнэ. 6 секундын дараа Refresh:
`bd1-L0-0` улаан (Z) болж, «Өөрчлөлт»-д `~ PID bd1-L0-0 S→Z`. «Зогсоох» дарахад сервер wait() хийж zombie алга болно.

**Жишээ 3 — thread-ээс fork + SIGSTOP.** «Worker thread-ээс fork()» ба «Thread-уудыг харуулах»-ыг чагтлаад гүн 2, салбар 2-оор үүсгэнэ.
`bd1-L0-0`-ийн доор `bd1-worker` thread (тасархай ирмэг) ба `bd1-L1-*` хүүхдүүд харагдана — kernel тэднийг worker thread-ийн жагсаалтад хадгалдаг ч
модуль бүх thread-ийн `children`-ийг гүйдэг тул алдагдахгүй. SIGSTOP дарахад бүх `bd1-*` ягаан (T), «Өөрчлөлт»-д `S→T`; SIGCONT дарахад буцаад S.

**Жишээ 4 — DFS.** Root PID = 1, «Тоглуулах»: зангилаанууд сурах бичгийн DFS дарааллаар (эцэг → эхний хүүхдийн бүх дэд мод → дараагийн хүүхэд) гэрэлтэнэ; баруун талын хүснэгтийн шар мөр хамт шилжинэ.

**Түгээмэл алдаа ба шийдэл**

| Алдаа | Шалтгаан | Шийдэл |
| --- | --- | --- |
| UI: «/proc/tasktree олдсонгүй» | module ачаалаагүй | `sudo insmod src/kernel/tasktree.ko` |
| `make: *** /lib/modules/.../build: No such file` | header алга | `sudo apt install linux-headers-$(uname -r)` |
| `insmod: ERROR: could not insert module: Key was rejected by service` | Secure Boot | VM-д Secure Boot унтраах эсвэл `mokutil`-ээр гарын үсэг |
| `insmod: ... File exists` | аль хэдийн ачаалагдсан | `sudo rmmod tasktree` дараа нь дахин |
| `insmod: ... Invalid parameters` / `Invalid argument` | `show_threads=abc` гэх мэт | `show_threads=0` эсвэл `1` |
| `Invalid module format` | өөр kernel-ийн header-ээр build хийсэн | `make clean && make` тухайн машин дээр |
| `OSError: [Errno 98] Address already in use` | порт завгүй | `--port 8001` |
| Root PID улаан хүрээ | тэр PID дууссан | Reset эсвэл 0 |
| Дээд мөрөнд `TRUNCATED (cap N)` | `max_tasks` хүрсэн | `echo 16384 \| sudo tee /sys/module/tasktree/parameters/max_tasks` |
| `insmod: ... Invalid argument` (max_tasks) | 16..16384-ээс гадуур | муж дотор утга |

## 8.2 Operational заавар

1. **Clone:** `git clone <repo-url> tasklist-graph && cd tasklist-graph`
2. **Орчин:** Ubuntu 24.04 LTS, kernel `6.8.0-xx-generic`, gcc 13, GNU make 4.3, Python ≥ 3.8. Шалгах: `uname -r; gcc --version; python3 --version`
3. **Dependency:** `sudo apt install -y build-essential linux-headers-$(uname -r) python3` (Python-д pip сан шаардлагагүй)
4. **Build:** `make -C src/kernel` → `tasklist_linear.ko`, `tasklist_dfs.ko`, `tasktree.ko`. Цэвэрлэх: `make -C src/kernel clean`
5. **Run:**
   - Part I: `sudo dmesg -C; sudo insmod src/kernel/tasklist_linear.ko; sudo dmesg; ps -el; sudo rmmod tasklist_linear`
   - Part II: `sudo dmesg -C; sudo insmod src/kernel/tasklist_dfs.ko; sudo dmesg; ps -eLf; sudo rmmod tasklist_dfs`
   - UI: `sudo insmod src/kernel/tasktree.ko [show_threads=1] [max_tasks=4096]; python3 src/ui/server.py`
   - Параметр ажиллаж байхад: `echo 1 | sudo tee /sys/module/tasktree/parameters/show_threads`,
     `echo 16 | sudo tee /sys/module/tasktree/parameters/max_tasks` (16..16384, гадуур бол татгалзана)
6. **Үндсэн тестийн өгөгдөл:** `examples/qemu/` (QEMU дахь Linux 6.8.0-31-ээс: `snap_workload.json` + `ps_workload.txt`, `snap_stopped.json`, `snap_capped.json`, `snap_after.json`, `snap_old.json`, `dmesg_part1/2.txt`, `results.txt`), мөн `examples/snapshot*_kernel.json`
7. **Нэмэлт boundary/error тест:** `bash tests/run_tests.sh` (23 тест), `bash scripts/qemu_test.sh [vmlinuz] [хуучин tasktree.ko]` (бодит kernel дээр 19 шалгалт),
   `python3 tests/compare_ps.py` (module ачаалсны дараа: DFS + LINEAR бүрэн байдал + ps),
   `sudo insmod src/kernel/tasktree.ko show_threads=abc` (→ Invalid argument), давхар `insmod` (→ File exists),
   UI-д Root PID=999999, интервал=0, гүн=6 оруулах.
8. **Хүлээгдсэн үр дүн:** Part I-ийн мөрийн тоо ≈ `ps -el`-ийн мөрийн тоо (зөрүү нь `ps`, `insmod` мэт богино процесс);
   Part II = Part I + 1 (PID 0 swapper); `compare_ps.py` → DFS OK, таарц ≥ 95 %; UI Refresh бүрт snapshot # +1.
9. **Known issues:** `children` жагсаалтыг `tasklist_lock`-гүйгээр (module-д export хийгдээгүй) `rcu_read_lock()`-оор уншдаг тул
   процесс яг тэр агшинд үүсч/дуусвал маш ховор тохиолдолд нэг task алдагдаж/давхардаж болно (`max_tasks` нь давталтыг хязгаарлана; `stop_machine()`
   ашиглавал бүрэн тогтвортой ч бүх CPU-г зогсоодог тул сонгоогүй). `tasktree` нь рекурсгүй (тодорхой стек); Part II-ийн `tasklist_dfs` номын загвараар
   рекурс хэвээр, гүн 256-д хязгаарлагдсан. `comm` нь 15 тэмдэгтээр тасардаг (kernel-ийн хязгаар).
