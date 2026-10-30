# Buddy 与 SLUB 合并检查

## 合入范围

- Buddy 新版两个文件、默认关闭的验证开关；保留旧 Buddy 文件但不选择旧管理器，两套导出符号不同，不会仅因并存而冲突。
- SLUB 两个文件和 kern_init 中的初始化、自测调用。
- 保留组长 page_init 全部函数体、课程评分脚本、现有 entry.S 和 Makefile。
- 组员新版 QEMU 启动适配及辅助评分脚本不合入当前 QEMU 4.1.1 工程。

## 已修正

- SLUB 大对象 size + 16 及向上对齐运算增加溢出检查，失败返回 NULL。
- 同一边界测试增加 SIZE_MAX 附近申请、free(NULL)、2049 字节直通分配。
- 只把空闲对象链头操作称为 O(1)；分配扫描可用 slab，释放扫描各 cache/slab，不能宣称整体为 O(1)。
- RV64 下 slub_slab 字段合计 40 字节；页首仍保留 64 字节。

## 测试覆盖的实际限制

- Buddy split_merge 测试的两个半块不保证来自唯一同一个父块；之后能再申请大块不能独立证明完整多级合并。固定 4096 页的耗尽用例只证明该大小请求失败，不表示全部单页已用尽。
- Buddy free 接口要求来自合法分配的块头和原请求长度；不支持任意指针、重复释放或任意错误长度。
- SLUB 的 guard 用例写入合法对象范围，检查邻居前一个机器字及 canary；不是主动越界写或通用越界防护测试。
- SLUB 混合负载使用固定种子的伪随机序列，是有限样例。代码不是每次操作均调用结构检查。
- SLUB 释放未经登记的大对象指针时直接读取头部；非法指针可能先产生访问异常，不能承诺必然被 magic 断言安全拦截。重复释放也不受支持。
- slub_init 的重入前提是没有存活分配。当前测试仅在空 cache 上重建；没有并发、抢占或 SMP 支持。
- 自测成功不代表所有边界都被覆盖。课程 make grade 仍由组长在 VS Code 运行并补正式截图。

## 验证

课程裸机 GCC 10.2.0、QEMU 4.1.1 下的预检副本编译及启动自测通过：

- 默认 Best-Fit + SLUB：SLUB 九项 OK，`all slub checks passed`，初始空闲页 31928，空闲区起点 0x80348000。
- Buddy + SLUB（`make DEFS=-DLAB2_VERIFY_BUDDY_PMM=1`，前后清理重建）：Buddy 五项 OK，SLUB 九项 OK；初始空闲页 31927，空闲区起点 0x80349000。配置变化会影响链接后的内核大小，页数应以本次输出为准。
- 测试末尾均检查页数及结构恢复；随后已重新构建默认 Best-Fit。
- 每次启动观察 8 秒；测试完成后内核进入循环，由 timeout 结束模拟机，终止信息不表示自测失败。
- 构建时有 Windows/WSL 文件时间差导致的 Clock skew 警告，但链接和启动自测均完成。
- **没有运行课程 make grade。正式评分和截图仍由组长在 VS Code 完成。**

原始启动日志：[Best-Fit + SLUB](./logs/20261030-bestfit-slub.txt)、[Buddy + SLUB](./logs/20261030-buddy-slub.txt)。

评分时须通过 make 命令行显式指定 GCCPREFIX（原 Makefile 的赋值会覆盖同名环境变量）。
