# tools 里的复现脚本

这些脚本只用于复现与自查，不参与评分；其中 `grade_new_qemu.sh` 的结果是本地自查评分，正式成绩以课程 `make grade` 为准。

| 脚本 | 用途 |
| --- | --- |
| `slub_check.sh` | `make clean` → `make -j2` → 以 `-device loader,file=bin/ucore.img,addr=0x80200000` 启动 QEMU 4.1.1，查看九行 `[slub] ... OK` 与 `[slub test] all slub checks passed` |
| `buddy_check.sh` | 把 `LAB2_VERIFY_BUDDY_PMM` 改成 1、重编、启动 QEMU，看五行 `[buddy] ... OK` |
| `restore_best_fit.sh` | 把开关改回 0，并清理、重新编译 |
| `grade_new_qemu.sh` | 新版 QEMU（固件不再带 `fw_jump`）下的本地自查评分：复制工程 → `tools/inject.py` → `-kernel` 启动 → 抓两行 `PASS` |
| `check_env.sh` | 检查 RISC-V 工具链、QEMU、make、python3 是否就绪，并试编译 |
| `verify_pmm_logic.py` | First-Fit / Best-Fit / Buddy 的分配逻辑模型自测，不代表 C 实现逐行等价 |
| `verify_slub_logic.py` | SLUB 的分配逻辑模型自测，另含伪随机负载 |

用法示例：

```sh
sh tools/check_env.sh          # 先确认环境
sh tools/slub_check.sh         # 看 SLUB 自测输出（Ctrl+C 退出）
sh tools/buddy_check.sh        # 看 Buddy 自测输出（Ctrl+C 退出）
sh tools/restore_best_fit.sh   # 回到提交态（开关 = 0）
sh tools/grade_new_qemu.sh     # 本地自查评分
python3 tools/verify_slub_logic.py
```

`grade_new_qemu.sh` 内部直接运行 `make`，仅设置环境变量 `GCCPREFIX` 不足以覆盖 Makefile 的赋值。使用前应将所需裸机工具链的 bin 目录加入 PATH。课程 QEMU 4.1.1 的评分使用 `make GCCPREFIX="工具链前缀" QEMU="模拟器路径" grade`。Python 脚本仅作模型自测；SLUB 模型使用简化页分配，元数据与 canary 的存储方式、整数溢出及边界测试均与 C 实现存在差异。
