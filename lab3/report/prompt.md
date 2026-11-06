# Lab3 提示词与接口约束

本文件汇总报告第四节的提交用提示词。模块一根据严宗贺提供的提示词与最终源码核对；中断部分来自田城昊的实现记录；异常部分由李文浩根据最终实现和验证整理。它们是提交时整理后的规格，不作为未经编辑的原始对话记录。

## 2511203 严宗贺：模块一

以下按严宗贺提交的提示词与最终源码核对整理，明确最终接口和恢复约束：

````markdown
[PROMPT]
任务：在 riscv64-ucore 的 kern/trap/trapentry.S 中补全 .macro RESTORE_ALL，
并把 __alltraps 与 C 函数 trap() 接起来。
操作要求：必须修改项目中的真实文件；不得改变已给定的 SAVE_ALL 的现场布局、struct trapframe
和 trap() 的签名；保留 #lab3 function1 标记并填入学号2511203。
输出要求：只给汇编实现与必要的注释，不引入与 Trap 返回无关的功能。

[RELY]
- SAVE_ALL（保留原保存逻辑和布局）：csrw sscratch, sp → addi sp, sp, -36*REGBYTES →
  STORE x0..x31（跳过 x2）→ csrrw s0, sscratch, x0 → csrr s1 sstatus / s2 sepc /
  s3 stval（旧名sbadaddr）/ s4 scause → STORE s0→2*REGBYTES、s1→32、s2→33、s3→34、s4→35。
- 宏：REGBYTES=8、LOAD=ld、STORE=sd；CSR 写回用 csrw。
- 结构体：struct trapframe { struct pushregs gpr; uintptr_t status, epc, badvaddr, cause; }
- 返回：__trapret 里执行 RESTORE_ALL 后 sret；sret 用 sepc 作返回 PC、用 sstatus.SPP
  恢复特权级、用 SPIE 恢复 SIE。
- 调用约定：按 RISC-V psABI，第一个参数放 a0，ra 由 jal 覆盖。

[GUARANTEE]
必须实现：.macro RESTORE_ALL 全部指令；__alltraps 中 SAVE_ALL 之后的传参与调用两行。
保留 SAVE_ALL 的逻辑；允许将旧CSR别名sbadaddr改为等义名称stval。不得改动 trap() 签名或 __trapret 的 sret。

[SPECIFICATION]
Pre-Condition:
- 进入 __alltraps 时 sp 指向有效的当前内核栈；SAVE_ALL 首先把旧sp写入sscratch，再分配Trap frame。
- 进入 __trapret 时 sp 仍等于 SAVE_ALL 之后的 &tf。

Post-Condition:
- 按 SAVE_ALL 布局恢复31个可写通用寄存器与4个CSR；x0恒为0，无需加载槽位0。未被处理函数修改的通用寄存器应与进入时一致。
- sstatus/sepc/stval/scause 与槽位 32~35 相同；其中 sepc 必须取自 tf->epc。
- 执行 sret 后 PC = sepc，栈指针回到进入 Trap 之前的值。

Case 1（正常返回）：先写 4 个 CSR（借 s0 作临时寄存器），再按 1..31 恢复 GPR，
最后一条才 LOAD x2。
Case 2（x0）：无需恢复；对x0的写入不会改变其恒零值。
Case 3（sp）：必须在所有以 sp 为基址的 LOAD 之后恢复，否则 trapframe 基址丢失。

Requirements:
- RESTORE_ALL 的槽位顺序必须与 SAVE_ALL 逐一镜像，不得假设别的布局。
- __alltraps 里 SAVE_ALL 之后 sp 就是 &tf，直接 mv a0, sp 传参。
- jal trap 覆盖 ra 是安全的：ra 已在槽位 1 中保存，返回时会被恢复。
````

## 2510582 田城昊：中断处理

本次使用的完整 spec 和提示词保存在 [prompt.md](./prompt.md)。核心提示词为：

```text
[ROLE]
你是 RISC-V uCore 实验助手，只实现 Lab3 田城昊负责的中断处理函数。

[CONTEXT / RELY]
读取 trap.c、trap.h、riscv.h、sbi.h、clock.c/.h 和课程评分代码。
框架提供 TICK_NUM=100、print_ticks()、volatile ticks、
clock_set_next_event()、sbi_shutdown()、print_trapframe() 和 IRQ_* 宏。
SHUTDOWN_AFTER 缺失，本次默认补为 10，允许编译参数覆盖。

[SPECIFICATION]
实现 void interrupt_handler(struct trapframe *tf)。清除 cause 最高位后
识别编号，保留原 Trap frame。IRQ_S_TIMER 先预约下一事件，再递增 ticks；
每 100 次调用 print_ticks()，累计输出达到阈值时调用 sbi_shutdown()。
其他已定义的软件、时钟、外部中断输出类型提示。未知中断输出
Unknown interrupt: cause %lu 并调用 print_trapframe(tf)。保留评分要求的
Supervisor software interrupt 字符串。不要调整 tf->epc。

[CONSTRAINTS]
只填中断待实现区域，保留异常和汇编待填位置，先验证缺失行为再实现。
记录真实结果，区分独立函数测试与带依赖的整机验证。
```

## 2514231 李文浩：异常处理

以下提示词根据最终实现和验证过程整理。

````markdown
[TASK]
在 kern/trap/trap.c 中实现 exception_handler(struct trapframe *tf)。

[RELY]
使用现有的 struct trapframe、CAUSE_ILLEGAL_INSTRUCTION、
CAUSE_BREAKPOINT、cprintf() 和 print_trapframe()。返回汇编会把
Trap frame 中的 epc 恢复到 sepc。

[SPECIFICATION]
识别非法指令和断点，分别打印 Illegal instruction at 或 Breakpoint at，
并输出触发异常的地址。读取该地址处的首个 16 位半字，判断本实验所用
指令的长度，再将 tf->epc 前移 2 或 4 字节。其他异常只打印 Trap frame。

[VERIFICATION]
分别触发 4 字节非法指令、4 字节 ebreak 和 2 字节 c.ebreak，
检查异常日志和返回后的 returned 日志。合入后运行 make grade。
````

