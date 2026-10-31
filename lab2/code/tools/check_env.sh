#!/bin/sh
# Lab2 环境自检脚本
# 用法：在 WSL / Ubuntu 里，进入 lab2_new 目录后执行
#     sh tools/check_env.sh
# 它会检查 make grade 需要的全部工具，并尝试真正编译一次内核。

fail=0
ok()   { printf '  [ OK ]   %s\n' "$1"; }
bad()  { printf '  [FAIL]   %s\n' "$1"; fail=$((fail + 1)); }
warn() { printf '  [WARN]   %s\n' "$1"; }
title(){ printf '\n=== %s ===\n' "$1"; }

title "系统信息"
printf '  %s\n' "$(uname -srm 2>/dev/null || echo 未知)"
printf '  当前目录: %s\n' "$(pwd)"
if [ -f Makefile ] && [ -d kern/mm ]; then
    ok "看起来在 lab2 工程根目录"
else
    bad "这里不是 lab2 工程根目录（缺少 Makefile 或 kern/mm），请 cd 到 lab2_new 再运行"
fi

title "1. RISC-V 交叉编译工具链（必须是 riscv64-unknown-elf-*，不能用 riscv64-linux-gnu-*）"
if command -v riscv64-unknown-elf-gcc >/dev/null 2>&1; then
    ok "riscv64-unknown-elf-gcc: $(riscv64-unknown-elf-gcc --version 2>&1 | head -n 1)"
else
    bad "找不到 riscv64-unknown-elf-gcc"
    if command -v riscv64-linux-gnu-gcc >/dev/null 2>&1; then
        warn "存在 riscv64-linux-gnu-gcc，但它会生成 GOT/PIC 寻址，导致内核入口阶段没有任何输出（报告 3.1 节踩过这个坑）"
    fi
fi
command -v riscv64-unknown-elf-ld      >/dev/null 2>&1 && ok "riscv64-unknown-elf-ld"      || bad "找不到 riscv64-unknown-elf-ld"
command -v riscv64-unknown-elf-objcopy >/dev/null 2>&1 && ok "riscv64-unknown-elf-objcopy" || bad "找不到 riscv64-unknown-elf-objcopy"
command -v riscv64-unknown-elf-objdump >/dev/null 2>&1 && ok "riscv64-unknown-elf-objdump" || bad "找不到 riscv64-unknown-elf-objdump"
if command -v riscv64-unknown-elf-gdb >/dev/null 2>&1; then
    ok "riscv64-unknown-elf-gdb（做 gdb 分支任务时才需要）"
else
    warn "没有 riscv64-unknown-elf-gdb（只影响 gdb 分支任务，不影响 make grade）"
fi

title "2. QEMU（课程要求 4.1.1）"
if command -v qemu-system-riscv64 >/dev/null 2>&1; then
    qv="$(qemu-system-riscv64 --version 2>&1 | head -n 1)"
    ok "qemu-system-riscv64: $qv"
    case "$qv" in
        *4.1.1*) : ;;
        *) warn "版本不是 4.1.1，若 make qemu 异常可换回 4.1.1" ;;
    esac
else
    bad "找不到 qemu-system-riscv64"
fi

title "3. 构建与评测脚本依赖"
command -v make  >/dev/null 2>&1 && ok "make: $(make --version 2>&1 | head -n 1)" || bad "找不到 make"
command -v tar   >/dev/null 2>&1 && ok "tar（make grade 内部要用）"               || bad "找不到 tar"
if command -v python3 >/dev/null 2>&1; then
    ok "python3: $(python3 --version 2>&1)（make grade 会调用 tools/inject.py）"
else
    bad "找不到 python3（make grade 必需）"
fi
command -v gdb >/dev/null 2>&1 && ok "gdb（调试 QEMU 源码时需要）" || warn "没有宿主 gdb，不影响 make grade"

title "4. 试编译一次内核"
if command -v make >/dev/null 2>&1 && command -v riscv64-unknown-elf-gcc >/dev/null 2>&1; then
    make clean >/dev/null 2>&1
    if make -j2 >/tmp/lab2_build.log 2>&1; then
        if [ -f bin/ucore.img ]; then
            ok "编译成功，已生成 bin/ucore.img（$(wc -c < bin/ucore.img) 字节）"
        else
            bad "make 返回成功但没有 bin/ucore.img"
        fi
    else
        bad "编译失败，最后 20 行日志："
        tail -n 20 /tmp/lab2_build.log | sed 's/^/           /'
    fi
else
    warn "跳过编译（缺 make 或交叉编译器）"
fi

title "结论"
if [ "$fail" -eq 0 ]; then
    printf '  环境可用。接下来依次执行：\n'
    printf '    make qemu    # 看输出；验证 Buddy 时先把 kern/mm/pmm.c 的 LAB2_VERIFY_BUDDY_PMM 改成 1\n'
    printf '    make grade   # 期望 Total Score: 20/20\n'
else
    printf '  有 %d 项不满足，先解决上面的 FAIL 项。\n' "$fail"
fi
exit "$fail"
