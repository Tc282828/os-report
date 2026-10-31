#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
GCCPREFIX="${GCCPREFIX:-/home/lenovo/tools/Risc-V/riscv64-unknown-elf-toolchain-10.2.0-2020.12.8-x86_64-linux-ubuntu14/bin/riscv64-unknown-elf-}"
QEMU="${QEMU:-/home/lenovo/src/qemu-4.1.1/build/riscv64-softmmu/qemu-system-riscv64}"
make GCCPREFIX="$GCCPREFIX" clean
make GCCPREFIX="$GCCPREFIX" -j2
exec "$QEMU" -machine virt -nographic -bios default -device loader,file=bin/ucore.img,addr=0x80200000
