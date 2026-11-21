# Lab 4 提示词

## 1. 严宗贺

```text
[PROMPT]
在课程的 Lab 4 起始工程中补齐模块一。修改 kern/mm/pmm.c 中虚拟内存页操作及其直接依赖。核对 Lab 2／3 标记块，使用小组上轮的已验证源码作为移植依据。完成实际文件修改，保留课程标记并把模块一的 YOUR CODE 替换为 2511203。保持进程创建、调度和评分文件的课程接口。

[RELY]
系统使用 RV64 Sv39。PDX1、PDX0、PTX 定位三级页表，PGSIZE 是 4096。
get_pte(pde_t *pgdir, uintptr_t la, bool create) 返回叶槽的内核虚拟地址，create 决定是否允许分配下级页表。
struct Page 的 ref 由 page_ref_inc、page_ref_dec 管理。使用 page2ppn、pte2page、pte_create、free_page、tlb_invalidate 操作现有页表及物理页。
依赖来源为小组 Lab 2／3 源码中的物理页初始化、页分配器、中断与异常处理和 Trap 汇编。Trap 布局由 Lab 4 trap.h 约定，首次进入新线程由 Lab 4 的 forkrets 接到 __trapret。

[GUARANTEE]
int page_insert(pde_t *pgdir, struct Page *page, uintptr_t la, uint32_t perm);
void page_remove(pde_t *pgdir, uintptr_t la);
同时补齐课程标记处的 Lab 2／3 依赖，保持现有头文件和调用 ABI。

[SPECIFICATION]
输入为有效页表根及受物理页管理器管理的物理页。映射以 4 KiB 叶页为单位。
插入成功返回 0，目标虚拟页指向指定物理页，叶权限随 perm 更新并采用合法的 Sv39 叶编码。同页换权限保证最终引用数和空闲页数保持原值，换页保证旧页少一个引用而新页多一个引用。
分配失败返回 -E_NO_MEM，原映射和数据页引用保持原状态，临时分配且尚未挂接的页表应归还。
删除已有映射时清空叶槽并撤销一条引用，最后一次引用消失时归还物理页。删除缺失映射及重复删除均无副作用。
新挂接非叶项以后同步当前 hart 上的地址翻译，非叶项的 U/A/D 位保持为零。写入或清除叶项后针对对应虚拟地址同步，再释放旧页引用。
验证时查看 check_alloc_page、check_pgdir、check_boot_pgdir 和评分中的 Test PMM。结果只记录实际运行输出。
```

## 2. 田城昊

```text
[PROMPT]
在 lab4_new/kern/process/proc.c 中完成 2510582 负责的进程创建与初始化。
保留已有的 alloc_proc()，接着实现 copy_mm()、copy_thread()、
do_fork()、kernel_thread() 和 proc_init()。

[RELY]
proc.h 给出 PCB 和 context 的字段；trap.h 和 trapentry.S 约定 trapframe
的布局。forkrets 把 sp 指向 trapframe 后走 __trapret。process/entry.S
用 s0 里的函数地址和 s1 里的参数调用线程函数，再进入 do_exit()。
复用 get_pid()、hash_proc()、setup_kstack()、put_kstack() 和 wakeup_proc()。
当前阶段只有内核线程，它们共用启动页表，mm 保持 NULL。

[GUARANTEE]
保持 proc.h 中的接口，以及 proc.c 内部辅助函数的作用域。
保留 LAB4 function2 标记并填入学号 2510582。
调度、页表切换和寄存器切换由模块三实现。

[SPECIFICATION]
alloc_proc() 分配 PCB，初始 PID 为 -1，状态为 PROC_UNINIT。
proc_init() 初始化进程链表及 PID 哈希桶，把 bootstack 交给 PID 0 的
idleproc，设为 current 并请求调度，再创建 PID 1 的 initproc。
kernel_thread() 将函数和参数写入 s0、s1，将 epc 指向 kernel_thread_entry，
设置 SPP、SPIE 并清除 SIE。copy_thread() 把现场复制到新内核栈，
将子线程的 a0 置零，把 context.ra、context.sp 分别设为 forkret 和现场地址。
do_fork() 先分配 PCB 和内核栈，再准备地址空间与现场。关中断期间分配 PID、
登记并唤醒线程。失败按逆序释放已分配资源，返回负错误码。
先用课程 PMM 和 Process Create 用例验证。完整调度测试等待模块三合入，
报告中只写实际观察到的输出。
```

## 3. 李文浩

```text
[PROMPT]
完成 Lab 4 模块三，学号 2514231。实现 proc.c 中的 cpu_idle、proc_run，
sched.c 中的 schedule，以及 process/switch.S 中的 switch_to。
保留其他两位同学的页表和进程创建代码，保持 LAB4 function3 标记。

[RELY]
proc.h 的 context 按顺序包含 ra、sp、s0～s11，RV64 下每项 8 字节。
idleproc 不在普通进程链表内；current 指向当前线程。
模块二将 pgdir 初始化为 boot_pgdir_pa，将新线程 context.ra 设为 forkret，
context.sp 设为线程的 trapframe。复用现有中断保护、链表和 TLB 接口。

[GUARANTEE]
保持四个函数的现有接口。proc_run 接收 PCB 指针，switch_to 接收两份
context 的指针。页表切换使用物理页号和 Sv39 模式，不使用 Sv32 的 lsatp。

[SPECIFICATION]
cpu_idle 持续检查 need_resched，非零时调用 schedule。
schedule 关本地中断后清除调度请求，从当前节点之后循环查找 RUNNABLE
线程，跳过表头；idleproc 从表头开始查找。无可运行线程时回退 idleproc。
正确处理只有当前线程可运行的情况，更新 runs 后调用 proc_run。
proc_run 在目标不同于当前线程时保存 prev，更新 current，写 satp 并刷新
TLB，再切换 context；关键区使用 local_intr_save/restore 保护。
switch_to 按结构体偏移用 sd/ld 保存恢复 14 个寄存器，以 ret 继续执行。
验证完整编译、课程 PMM/Process Create/Schedule 用例，以及 initproc 输出。
```
