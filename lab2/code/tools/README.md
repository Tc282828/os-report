# tools 里的复现脚本

这些脚本只用于复现与自查，不参与评分；其中 `grade_new_qemu.sh` 的结果是本地等价评分，正式成绩以课程 `make grade` 为准。

| 脚本 | 用途 |
| --- | --- |
| `slub_check.sh` | `make clean` → `make -j2` → 以 `-kernel` 启动 QEMU，直接看九行 `[slub] ... OK` 与 `[slub test] all slub checks passed` |
| `buddy_check.sh` | 把 `LAB2_VERIFY_BUDDY_PMM` 改成 1、重编、启动 QEMU，看五行 `[buddy] ... OK` |
| `restore_best_fit.sh` | 把开关改回 0、重编，并打印开关值与 `bin/` 产物，确认提交态 |
| `grade_new_qemu.sh` | 新版 QEMU（固件不再带 `fw_jump`）下的本地等价评分：复制工程 → `tools/inject.py` → `-kernel` 启动 → 抓两行 `PASS` |
| `check_env.sh` | 检查 RISC-V 工具链、QEMU、make、python3 是否就绪，并试编译 |
| `verify_pmm_logic.py` | 把 First-Fit / Best-Fit / Buddy 逐行复现成 Python，跑同一套断言（本机没有 RISC-V 环境时先用它排逻辑错误） |
| `verify_slub_logic.py` | 同样方式复现 SLUB 的九个用例并跑通 |

用法示例：

```sh
sh tools/check_env.sh          # 先确认环境
sh tools/slub_check.sh         # 看 SLUB 自测输出（Ctrl+C 退出）
sh tools/buddy_check.sh        # 看 Buddy 自测输出（Ctrl+C 退出）
sh tools/restore_best_fit.sh   # 回到提交态（开关 = 0）
sh tools/grade_new_qemu.sh     # 本地等价评分
python3 tools/verify_slub_logic.py
```
