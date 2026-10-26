# Linux Kernel Module for Listing Tasks + Task Graph UI

**F.CSM302 Үйлдлийн систем — Бие даалт 1**
Хуваарилалтын код: **II-4-3**

| Гишүүн | Оюутны код | 
| --- | --- | 
| М. Хуншагай | B242270134 | 
| Т. Төгөлдөрчимэг | B241940117 | 
| Г. Тэргэл | B242270048 | 
| Б. Дэлгэрмөрөн | B210930009 | 
| Г. Мөнхнайрамдал | B241910043 | 
| П. Даваажаргал | B231940145 | 

Сурах бичиг: Silberschatz, Galvin, Gagne — *Operating System Concepts* 10e (Wiley, 2018),
**Бүлэг 3 Processes → Programming Project 3 "Linux Kernel Module for Listing Tasks"**.
UI дүрслэл: **Task graph**.

Kernel module-оор системийн бүх task-ыг (1) `for_each_process()`-оор шугаман, (2) `init_task`-аас
`children`/`sibling` жагсаалтаар DFS-ээр гүйж жагсаана. Өргөтгөл болох `tasktree` module нь
`/proc/tasktree` файлыг уншиx бүрт шинэ snapshot-ыг JSON-оор гаргаж, вэб UI нь
эцэг–хүүхдийн холбоог граф хэлбэрээр харуулна. **Refresh дарах бүрт бодит жагсаалт шинэчлэгдэнэ.**

## Туршсан орчин

| Орчин | Kernel | Compiler | Үр дүн |
| --- | --- | --- | --- |
| **Ubuntu 26.04.1 LTS (үндсэн, багийн VM)** | 7.0.0-42-generic | GCC 15 | QEMU 19/19 PASS — `examples/qemu-ubuntu-26.04/` |
| Ubuntu 24.04 LTS (нэмэлт шалгалт) | 6.8.0-31-generic | GCC 13 | QEMU 19/19 PASS — `examples/qemu/` |

Код нь `LINUX_VERSION_CODE`-оор хувилбарын ялгааг шийддэг тул 5.14-өөс дээш kernel дээр өөрчлөлтгүй build хийгдэнэ.
Өөр kernel дээр ажиллуулахдаа `make -C src/kernel clean && make -C src/kernel` гэж тухайн машин дээрээ дахин build хийнэ.

## Quick Start (Ubuntu 26.04 VM; 24.04 дээр ч адилхан)

```bash
sudo apt update && sudo apt install -y build-essential linux-headers-$(uname -r) python3
git clone https://github.com/B242270134/Uildliin_system_bd1 tasklist-graph && cd tasklist-graph
make -C src/kernel                       # 3 module build
sudo insmod src/kernel/tasklist_linear.ko && sudo dmesg | tail -20 && sudo rmmod tasklist_linear   # Part I
sudo insmod src/kernel/tasklist_dfs.ko    && sudo dmesg | tail -20 && sudo rmmod tasklist_dfs      # Part II
sudo insmod src/kernel/tasktree.ko                                                                   # UI-д
python3 src/ui/server.py                  # http://127.0.0.1:8000
```
Эсвэл бүгдийг нэг дор: `bash scripts/run_all.sh`

Module-гүй орчинд UI-г шалгах: `python3 src/ui/server.py --source userspace` эсвэл
`python3 src/ui/server.py --source file --file examples/snapshot1_kernel.json`.

## Бүтэц

```
src/kernel/   tasklist_linear.c (Part I), tasklist_dfs.c (Part II), tasktree.c (/proc JSON), common.h, Makefile
src/ui/       server.py (Python stdlib HTTP), index.html (SVG task graph)
tests/        run_tests.sh, compare_ps.py (DFS + LINEAR + ps шалгалт), forktree.py (fork мод, thread-ээс fork горим),
              bd1_threads.c, thread_fork.c
examples/     QEMU-д бодит kernel дээр авсан лог, JSON snapshot, ps гаралт:
              qemu-ubuntu-26.04/ (7.0.0-42), qemu/ (6.8.0-31)
docs/         MANUAL.md, img/ (UI screenshot)
scripts/      run_all.sh, qemu_test.sh (бодит kernel дээр автомат 19 шалгалт)
```

## Тест
```bash
bash tests/run_tests.sh            
python3 tests/compare_ps.py         
bash scripts/qemu_test.sh /boot/vmlinuz-$(uname -r)   
```

## Ашигласан эх сурвалж / AI
- OSC 10e, Ch.3 Programming Project 3; Linux kernel `<linux/sched.h>`, `<linux/list.h>`, `<linux/seq_file.h>`.
- Гадны сан ашиглаагүй (UI нь цэвэр JavaScript + SVG, сервер нь Python стандарт сан).
- AI туслах (Claude) ашиглан заавар бэлтгэсэн; 
