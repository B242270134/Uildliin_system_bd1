# Ubuntu 26.04.1 (kernel 7.0.0-42-generic) дээрх туршилт

- Kernel image ба header: Ubuntu `resolute-updates` архивын `linux-image-7.0.0-42-generic`, `linux-headers-7.0.0-42-generic` (7.0.0-42.42)
- Compiler: GCC 15.3 (kernel-ийг GCC 15.2-оор хөрвүүлсэн; GCC 13 нь `-fmin-function-alignment`-ийг танихгүй тул заавал GCC 15)
- `make -C src/kernel W=1`: 0 warning
- `bash scripts/qemu_test.sh /boot/vmlinuz-7.0.0-42-generic <хуучин tasktree.ko>`: **19/19 PASS** (`results.txt`)
- Snapshot-уудын `"kernel":"7.0.0-42-generic"` талбар нь kernel хувилбарыг баталгаажуулна.
