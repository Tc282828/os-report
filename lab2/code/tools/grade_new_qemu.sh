#!/bin/sh
# tools/grade_new_qemu.sh
#
# 用途：在"新版 QEMU"环境（其自带 OpenSBI 为 fw_dynamic，不认
#       -device loader,file=...,addr=... 的固定跳转地址）下，做一次与
#       课程 tools/grade.sh **逻辑等价**的本地评分，用于自查。
#
# 重要：这不是课程的官方 make grade。官方评分请在 QEMU 4.1.1 上执行 make grade，
#       报告里作为评分的截图也应当来自那次官方执行。
#
# 用法：sh tools/grade_new_qemu.sh
# 可选：QEMU=/path/to/qemu-system-riscv64 sh tools/grade_new_qemu.sh

TMP=/tmp/lab2_grade_local
LOG="$TMP/.serial.out"
QEMU_BIN="${QEMU:-qemu-system-riscv64}"

cd "$(dirname "$0")/.." || exit 1

echo "[1/5] 编译当前工程"
if ! make -j2 >/dev/null; then
    echo "!! 编译失败，先单独跑 make -j2 看报错"
    exit 1
fi

echo "[2/5] 复制工程并注入课程测试（tools/inject.py，与 make grade 同一步骤）"
rm -rf "$TMP"
mkdir -p "$TMP"
tar -C . --exclude=.git --exclude=.vscode --exclude=obj --exclude=bin -cf - . | tar -C "$TMP" -xf -
if ! python3 tools/inject.py "$TMP"; then
    echo "!! 注入失败（通常是 pmm.c 结构被改动导致定位不到函数）"
    exit 1
fi

echo "[3/5] 编译注入后的内核"
if ! ( cd "$TMP" && make -j2 >/dev/null ); then
    echo "!! 注入后的内核编译失败，到 $TMP 里单独 make 看报错"
    exit 1
fi

echo "[4/5] 启动 QEMU（-kernel 方式），15 秒后自动结束"
rm -f "$LOG"
( cd "$TMP" && timeout 15 "$QEMU_BIN" -machine virt -display none \
      -serial "file:$LOG" -monitor none -m 128M -bios default \
      -kernel bin/ucore.img >/dev/null 2>&1 ) || true

if [ ! -s "$LOG" ]; then
    echo "!! 串口没有任何输出。先跑 sh tools/check_env.sh，再确认内核能正常启动。"
    exit 1
fi

echo "[5/5] 统计结果"
echo
echo "===== 关键输出 ====="
grep -aE "memory management|physical memory|free physical memory|check_alloc_page|\[Test [12]\]" "$LOG" || tail -20 "$LOG"

p1=0
p2=0
if grep -aq "\[Test 1\] Page Init: PASS" "$LOG"; then
    p1=10
fi
if grep -aq "\[Test 2\] Best-Fit: PASS" "$LOG"; then
    p2=10
fi
score=$((p1 + p2))

echo
if [ "$p1" -eq 10 ]; then
    echo "check page init: OK      ($p1/10)"
else
    echo "check page init: WRONG   ($p1/10)"
    grep -a "\[Test 1\]" "$LOG" | tail -3
fi
if [ "$p2" -eq 10 ]; then
    echo "check best fit:  OK      ($p2/10)"
else
    echo "check best fit:  WRONG   ($p2/10)"
    grep -a "\[Test 2\]" "$LOG" | tail -3
fi
echo
echo "Total Score: $score/20"
echo
echo "（本地等价评分，用于自查；官方结果请在 QEMU 4.1.1 上执行 make grade）"
echo "（完整串口日志：$LOG）"
