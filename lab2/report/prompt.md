# Lab2 提示词汇总

本文件汇总 page_init、Best-Fit、Buddy 和 SLUB 的六份实现与测试提示词。page_init 提示词为实验后的复盘整理，其余提示词由对应成员提供。

## 1. 模块一任务一：page_init（2510582 田城昊）

```text
[PROMPT]
任务：修改 kern/mm/pmm.c，实现 page_init() 的物理内存探测、Page 描述符初始化和空闲物理页注册。
操作要求：必须修改项目中的真实文件；保留现有 pmm_init()、测试逻辑和 lab2 标记；不要修改公开接口，也不要硬编码某次运行得到的 free_start。
输出要求：仅完成该功能所需的代码；以工程内真实宏、数据结构和函数为准，不引入无关功能。

[RELY]

- DTB 接口：uint64_t get_memory_base(void)、uint64_t get_memory_size(void)。
- 页面大小与换算：PGSIZE、PPN、ROUNDUP、ROUNDDOWN。
- 地址关系：DRAM_BASE、PHYSICAL_MEMORY_OFFSET、PADDR。
- 全局状态：struct Page *pages、size_t npage、const size_t nbase、uint64_t va_pa_offset。
- struct Page 字段及操作：flags、property、page_link、set_page_ref、SetPageReserved。
- 可用接口：panic、cprintf、list_init、pa2page、init_memmap。
- 链接符号 end 是内核镜像末尾的虚拟地址。

[GUARANTEE]
必须实现：static void page_init(void)。
允许在 kern/mm/pmm.c 内增加局部变量；不得修改 get_memory_base()、get_memory_size()、pa2page() 或 PMM manager 的公开接口。

[SPECIFICATION]
Pre-Condition:

- DTB 已完成解析，PMM manager 已完成 init。
- 内核高半映射可用，end、PADDR 和 PHYSICAL_MEMORY_OFFSET 的含义有效。

Post-Condition:

- 正确设置 va_pa_offset、npage 和 pages。
- [nbase, npage) 的每个物理页都有对应描述符，初始均不可分配。
- 只将描述符数组之后到物理内存末端之间的完整页面注册为空闲区。

Case 1（有效范围且存在空闲完整页）：

- 检查地址范围不为空且加法不溢出。
- 将 pages 放在 end 之后的页对齐地址。
- 将全部 Page 描述符初始化为 Reserved、引用计数为 0、property 为 0。
- 计算描述符数组后的页对齐物理地址，调用 init_memmap() 注册空闲页。

Case 2（DTB 信息无效）：

- 对基址或大小为 0、范围溢出、DRAM 基址与平台约定冲突等情况调用 panic。

Case 3（没有可用完整页）：

- 保持所有描述符为 Reserved，不得用 0 页或无效范围调用 init_memmap()。

Requirements:

- 严格区分虚拟地址、物理地址、物理页号和 Page *。
- 起始地址向上对齐，物理内存末端向下对齐。
- 内核镜像与 Page 描述符数组占用的页面必须保持 Reserved。
```

## 2. 模块二任务二：Best-Fit（2514231 李文浩）

```text
[PROMPT]

给lab2补充一个bestfit物理内存分配器，写在新文件 kern/mm/best_fit_pmm.c 里。

模仿kern/mm/default_pmm.c ，其中初始化、登记空闲页、分配、释放、查询剩余页数的五个函数都保留，函数名加 best_fit_前缀。文件里原有的两个测试函数不要动，也不要复制。
只改分配：first-fit 是找到第一个够大的就 break，best-fit 要把整条链表扫完，选够大的里面最小的，大小一样就选地址低的。不要留 break。
注意default_pmm.c 里“插到链表最后”的分支少了一个 break，会把前驱指针指到自己，复制过来的时候补上。
文件末尾加管理器 best_fit_pmm_manager，检查函数用文件里现成的那个。
改 kern/mm/pmm.c 里初始化管理器那一行，换成新的。
其他文件不要动，page_init不用管。

[RELY]

参考实现：default_pmm.c 里五个函数就是现成的模板，结构、状态、合并方式都能照搬。
数据结构：页描述符（引用计数、标志位、块大小、链表节点）、空闲区（链表头 + 剩余页数）、管理器（名字 + 六个函数指针）。
链表：带哨兵的环形双向链表，有初始化、插入（分插到某节点之后/之前）、删除、判空、取前后节点这些操作。
页面操作：页大小与页号换算、保留位和空闲块标记的置/清/判、链表节点转页描述符、单页分配与释放、断言，工程里都有。
状态约定（测试函数依赖）：链表按地址升序、只挂空闲块首页；首页记录块大小并带标记；剩余页数等于各块大小之和；分配从块首切；释放立即合并左右。
[GUARANTEE]

必须交的有：
五个静态函数，名字加 best_fit_ 前缀（初始化、登记、分配、释放、查询）；
一个管理器常量 best_fit_pmm_manager，名字不能改，六个成员分别指向这五个函数和文件里现成的检查函数；
kern/mm/pmm.c 里初始化管理器那一行改成新管理器。
不要改两个测试函数，也不要改 default_pmm.c、pmm.h、memlayout.h、list.h。

[SPECIFICATION]

初始化
Pre：无。 
Post：链表为空，剩余页数为 0。 
Req：不碰页描述符。

登记空闲页
Pre：页数大于 0，起始页对齐，每一页都是保留页。 
Post：每页标志和引用计数清零；首页记录页数并打标记；剩余页数累加；按地址插入链表。 
Case 1：空表则插表头。 
Case 2：落在中间则插到第一个更大的块之前。 
Case 3：比所有块都大则插表尾，插完就结束循环。 
Req：保持有序；不合并。

分配
Pre：请求页数大于 0。 
Post：成功返回首页并摘下、清标记；块比请求大时，剩余部分以新首页插回原位；剩余页数减掉请求页数。失败返回空。
Case 1：请求页数超过剩余总数则直接返回空。 
Case 2：有够大的块则扫完整条链表，选够大的里面最小的（一样大取地址低的），从块首切。 
Case 3：都太小则返回空。 
Req：不要找到第一个就停，测试里的 assert(p0 + 4 == p1) 会挂；只减请求页数。

释放
Pre：页数大于 0；每页都不是保留页，且不在空闲链表上。 
Post：标志和引用计数清零；首页记录页数并打标记；剩余页数累加；按地址插回；与左右相邻块合并。 
Case 1：空表则插表头，不合并。 
Case 2：左邻接得上则并进左邻，代表块前移，再看右边。 
Case 3：右邻接得上则并进自己。 
Case 4：左右都接得上则一次并成一块。 
Req：先插回再合并；左右各看一个邻居；相邻要用“正好接上”判断；被吞并的块清标记并摘掉；合并不改剩余页数。

查询
Post：直接返回剩余页数，不要重新遍历。
```

## 3. 模块二任务三：Buddy 实现（2511203 严宗贺）

```text
[PROMPT]
任务：在 lab2 工程里从零新增一个 Buddy System 连续物理内存分配器，写入新文件 kern/mm/buddy_system_pmm.c 与 kern/mm/buddy_system_pmm.h，并接入内核。
操作要求：必须修改项目中的真实文件；不要改动 default_pmm.c、best_fit_pmm.c 及这两个文件里已有的测试函数；不要改动 tools/ 下的评测脚本；新增代码保留 lab2 注释标记，并在标记处写明学号。
输出要求：给出完整、可直接编译的 .c 与 .h；不引入无关功能；除管理器常量外不新增全局导出符号。

[RELY]

- 管理器接口：kern/mm/pmm.h 中 struct pmm_manager 的六个函数指针（init、init_memmap、alloc_pages、free_pages、nr_free_pages、check），以及 alloc_page()/free_page() 宏。
- 页描述符：kern/mm/memlayout.h 中 struct Page { int ref; uint64_t flags; unsigned int property; list_entry_t page_link; }，以及 SetPageProperty/ClearPageProperty/PageProperty、SetPageReserved/ClearPageReserved/PageReserved、le2page。
- 链表：libs/list.h 的 list_init、list_add、list_add_before、list_del、list_empty、list_next。
- 地址与页号：PGSIZE、PGSHIFT、PPN、page2pa/pa2page/page2ppn，全局 pages、npage、nbase。
- 对齐与断言：ROUNDUP、ROUNDDOWN、assert、panic、cprintf。
- 工程约定：kern/mm/*.c 会被 Makefile 自动编译；init_pmm_manager() 当前挂 best_fit_pmm_manager；课程评测的 [Test 2] 只认 best_fit_pmm_manager。

[GUARANTEE]
必须实现：上面六个静态函数，以及常量 const struct pmm_manager buddy_system_pmm_manager（名字不可改），六个成员分别指向这六个函数；头文件中声明该常量。
允许：在 kern/mm/buddy_system_pmm.c 内自行设计内部状态（按阶组织的空闲链表、计数器）与静态辅助函数。
不得修改：default_pmm.c、best_fit_pmm.c、pmm.h、memlayout.h、list.h、tools/*。
必须保证：提交状态下 init_pmm_manager() 仍然选择 best_fit_pmm_manager；Buddy 只能通过一个默认关闭的宏开关临时启用，并在注释中写明"验证时改 1、提交前改回 0"。

[SPECIFICATION]
Pre-Condition:
- init_memmap 由 page_init() 调用，传入的 [base, base + n) 每一页都已被置为 Reserved，且 n >= 1。
- npage、nbase、pages 已经由 page_init() 设置完成。

Post-Condition:
- init：所有阶链表为空，nr_free = 0。
- init_memmap：把区间拆成若干"起点按自身大小对齐、大小为 2^order"的块登记；被登记的页清除 Reserved、ref 与 property 清零；nr_free 增加 n。
- alloc_pages(n)：成功返回块头页——块按自身大小对齐、PageProperty 已清、块头 property 记录真实块大小 2^k，nr_free 减少 2^k；失败返回 NULL 且不改变任何状态。
- free_pages(base, n)：回收 size = max(块头 property, 向上取整到 2 的幂的 n) 个页，与同阶伙伴反复合并后挂回对应阶链表，nr_free 增加 size。
- nr_free_pages：直接返回计数器，不重新遍历链表。
- check：执行自带测试，任一断言失败即 panic。

Case 1（请求恰好是一整块）：直接摘下返回，不做分裂。
Case 2（请求小于最大可用块）：从最近的高阶块逐级二分，多出来的高半部分挂到低一阶链表。
Case 3（请求超过 nr_free 或超过单块上限）：直接返回 NULL。
Case 4（释放时伙伴块同阶、空闲且未被保留）：合并，阶数加一，继续向上判断。
Case 5（伙伴块被保留、阶数不同、或根本不在链表上）：停止合并，直接把当前块挂回链表。
Case 6（合并后块首前移到伙伴位置）：必须保证返回给上层的仍是合并后的块首，且块头记录的大小同步更新。

Requirements:
- 块大小恒为 2 的幂，块首必须按自身大小对齐（页号 % 2^k == 0）。
- 伙伴页号 = 本块页号 ^ 块大小；合并前必须同时确认伙伴块的 PageProperty 置位、property 等于该阶大小。
- 只有块头页挂链表并带 PageProperty；被合并掉的伙伴页要清标记并从链表摘除。
- nr_free 只按真实块大小增减，任何路径都不得让"各阶空闲块页数之和"与 nr_free 不一致。
- 分配出去后清除 PageProperty，但保留块头 property，作为释放时的真实块大小依据。
```

## 4. 模块二任务三：Buddy 测试（2511203 严宗贺）

```text
[PROMPT]
任务：为刚完成的 Buddy System 分配器设计并实现自带测试函数 buddy_check()，挂到 buddy_system_pmm_manager 的 check 成员上，随内核启动的 check_alloc_page() 执行。
操作要求：测试只能写在 kern/mm/buddy_system_pmm.c 内，不得修改 pmm.h、框架代码或其他分配器；不得依赖某一次运行得到的具体地址；每个用例结束时必须把分配器恢复到用例开始前的状态。
输出要求：测试函数 + 每个用例一行 cprintf 结论；断言失败立即 panic，不允许静默通过。

[RELY]

- 可用操作：alloc_pages/alloc_page、free_pages/free_page、nr_free_pages、page_ref、page2pa、page2ppn、PageProperty、PageReserved、assert、cprintf。
- 被测状态：buddy_area.free_list[order] 与 buddy_area.nr_free。
- 上文约定必须成立的不变量：块大小为 2^order、块首按自身大小对齐、块头 property 等于块大小、伙伴页号 = 页号 ^ 块大小。
- 已知布局：npage = 0x88000，nbase = 0x80000，空闲区约 3.2 万页，最大块阶约 14。

[GUARANTEE]
必须实现：static void buddy_check(void)，内部覆盖至少五类行为；允许增加静态辅助函数（例如遍历所有阶求空闲页数总和的"对账"函数）。
不得修改：default_pmm.c、best_fit_pmm.c 及其中已有的测试、tools/*。
不得改变内核启动后的内存状态：测试前后的空闲页总数必须一致。

[SPECIFICATION]
Pre-Condition：pmm_manager 已指向 buddy_system_pmm_manager，init 与 init_memmap 已完成，链表与计数器自洽。
Post-Condition：全部断言通过，状态与进入测试前一致，并输出各用例结论。

必须覆盖的用例：
Case 1（基本分配回收）：连续分配 3 个单页，三页互不相同、ref 为 0、nr_free 恰好减少 3；释放后 nr_free 与用例前完全相等。
Case 2（分配粒度）：请求 3 页，返回块必须 4 页对齐、块头 property 为 4、PageProperty 已清、nr_free 减少 4；按 3 页释放后计数还原。
Case 3（分裂与合并）：先从最小的空闲块整块分配再释放（验证不分裂路径）；再把最大的空闲块切成两半分别分配、然后分别释放，验证两半能重新合并成整块（合并后再请求整块必须成功）。
Case 4（越界请求）：请求页数大于全部受管物理页、以及大于 nr_free 时都必须返回 NULL，且 nr_free 与链表结构不变。
Case 5（耗尽路径）：以 4096 页为单位反复分配直到返回 NULL，记录成功次数，校验 nr_free 的减少量恰好等于已分配页数；再逆序全部释放，校验 nr_free 回到用例前的值。

Requirements:
- 每个用例前后都要用"遍历所有阶链表求页数和"的辅助函数与 nr_free 对账，并对每个链表成员校验：PageProperty 置位、块大小与所在阶一致、块首按自身大小对齐。
- 不得硬编码任何地址，所有判断只能依赖相对关系与不变量，保证内核体积变化时测试仍然成立。
```

模块一 Challenge 是思考题，双 GDB 分支是操作实验；当前没有可追溯的独立四段式实现提示词，因此不另行编造。实际操作及说明见 report.md。


## SLUB 实现与测试（2511203 严宗贺）

以下为 SLUB 实现与测试的两份提示词。

### SLUB 提示词 1

```text
[PROMPT]
任务：在 lab2 工程里从零新增一个两层结构的 SLUB 内存对象分配器，写入新文件 kern/mm/slub_pmm.c 与 kern/mm/slub_pmm.h，并在 kern/init/init.c 的 pmm_init() 之后接入（slub_init(); slub_check(); 两行，另加一行 #include <slub_pmm.h>）。
操作要求：必须直接修改工程中的真实文件；不得改动 kern/mm/pmm.c、default_pmm.c、best_fit_pmm.c、buddy_system_pmm.c 及它们已有的测试函数；不得改动 tools/ 下的评测脚本与 Makefile 的编译规则；新增代码保留 lab2 注释标记，并在标记处写明学号 2511203。
输出要求：完整、可直接编译的 .c 与 .h，以及 init.c 的改动片段（三行）；除头文件声明的七个接口外不新增导出符号；不使用浮点、不使用 malloc/free、不使用变长数组。

[RELY]

- 页级接口：kern/mm/pmm.h 中的 alloc_pages(size_t n)、free_pages(struct Page *base, size_t n)、nr_free_pages()、alloc_page()/free_page() 宏；struct Page 与 page2pa/pa2page/page2ppn；PADDR 宏；全局 pages、npage、nbase、va_pa_offset；PGSIZE=4096、PGSHIFT=12、PPN 宏。
- 工具：libs/assert.h 的 assert/panic、libs/stdio.h 的 cprintf、libs/string.h 的 memset、libs/defs.h 的 size_t/uintptr_t/ROUNDUP。
- 工程约定：kern/mm/\*.c 会被 Makefile 自动编译；编译选项为 -std=gnu99 -Wall -Werror -Wno-unused -msmall-data-limit=0，因此不能留下未使用的静态函数、不能有隐式声明、不能有符号/无符号不匹配的比较。
- 地址换算：内核虚拟地址 = 物理地址 + va_pa_offset（本机为 0xFFFFFFFF40000000）。KADDR 宏即使存在也不要在本模块里依赖它，直接用 page2pa(page) + va_pa_offset 自算。
- 提交态约束：课程评测的 [Test 2] 只认 best_fit_pmm_manager，本模块不得改动 pmm_manager 的选择，也不得在 pmm_init() 之前执行。

[GUARANTEE]
必须实现：与头文件完全一致的七个函数（slub_init、slub_alloc、slub_free、slub_nr_slabs、slub_nr_total_objects、slub_nr_inuse_objects、slub_check）。
允许：内部结构自定——档位表、每页元数据布局、直通路径头格式、空闲链实现方式都由实现者决定。
必须保证：① 元数据全部落在 slab 首页的前 64 字节内，并用编译期断言 sizeof(struct 元数据) <= 64 固定这一点；② slub_check() 结束时第一层页数必须与开始时完全一致；③ 不得在分配失败路径上留下半初始化的 slab（要么成功挂链，要么把页还回去）。

[SPECIFICATION]
Pre-Condition：

- 调用 slub_init() 时，pmm_init() 已完成，pmm_manager 已能正常分配页。
- slub_alloc(size) 的 size 是调用方真正要用的字节数，可能为 0，也可能远大于物理内存。
- slub_free(ptr) 的 ptr 必须是本模块此前返回的指针。

Post-Condition：

- slub_init：九个 cache 的 object_size、objects_per_slab 计算完成，slab 链与计数清零。
- slub_alloc(size)：size == 0 返回 NULL；size <= 2048 时返回某档位 slab 内的对象地址，且该对象已从 freelist 摘除、两层 inuse 各加一；size > 2048 时向页级层申请 ceil((size+16)/PGSIZE) 页，页首写 magic 与页数，返回头的后方地址；页级层返回 NULL 时本函数返回 NULL。
- slub_free(ptr)：ptr 为 NULL 时直接返回；落在某个 slab 的对象区内时把对象头插回 freelist、两层 inuse 各减一，inuse 归零则销毁 slab 并 free_pages 归还整页；不落在任何 slab 内时按直通路径读头、校验 magic、按头中页数归还，并把 magic 清零。
- slub_check：依次执行自带用例，全部通过后输出 [slub test] all slub checks passed，任一断言失败立即 panic。

Case 1（size = 0）：返回 NULL，不得返回内部指针，也不得改变任何计数。
Case 2（size 恰为档位值 8/16/…/2048）：归入该档，不得跳到下一档。
Case 3（size = 2049）：不能走对象层（会越界），必须走直通路径申请 2 页以内（ceil((2049+16)/4096) = 1 页）。
Case 4（单 slab 对象用尽）：新建一个 slab，绝不允许越过 slab 末端继续切分。
Case 5（释放 slab 内最后一个对象）：inuse 归零 → 销毁 slab → 页归还页级层，此时 slab 链上不得留下悬空节点。
Case 6（页级层分配失败）：slub_alloc 返回 NULL，已有 slab 与 freelist 不受影响，计数器不变。

Requirements：

- 分配与释放都必须是 O(1)（不含"扫描 slab 链找归属"这一层的常数级开销），不得遍历空闲链表求长度。
- 空闲对象链采用 LIFO：刚释放的对象应被下一次同档位分配优先复用。
- 元数据必须带哨兵 canary，取值固定；空闲对象的地址必须落在本 slab 的对象区内并按 object_size 对齐。
- 不变量：对任意 slab，freelist 长度 + inuse 必须恰好等于 objects_per_slab。
- 不得依赖任何具体虚拟地址或具体空闲页数（内核体积变化后必须仍然成立）。
```

### SLUB 提示词 2

```text
[PROMPT]
任务：为刚完成的 SLUB 分配器设计并实现自带测试：九个 static 测试函数 + 一个汇总函数 slub_check()，测试代码写在 kern/mm/slub_pmm.c 内部，由 kern_init() 调用一次后随内核启动执行。
操作要求：测试只能写在 slub_pmm.c 内，不得修改 pmm.h、其他分配器或框架代码；不得依赖某一次运行得到的具体地址与具体页数；每个用例结束时必须把第一层页数与对象计数恢复到用例开始前的状态，否则后续用例会在被污染的堆上运行。
输出要求：测试函数 + 每个用例一行 cprintf 结论（[slub] ... OK），汇总函数首尾各打印一行（[slub test] ...）；断言失败立即 panic，不允许只打印不检查。

[RELY]

- 可用操作：slub_alloc/slub_free、alloc_pages/free_pages/nr_free_pages、memset、assert、cprintf。
- 可读状态：slub_nr_slabs()、slub_nr_total_objects()、slub_nr_inuse_objects()，以及模块内部的 slab 链与 freelist。
- 已知常量：档位 9 个（8…2048）、SLUB_OBJ_OFFSET = 64、SLUB_SLAB_SIZE = 4096、2048 档每页恰好 1 个对象、64 档每页 63 个对象。
- 已知不变量：freelist 长度 + inuse == objects_per_slab；空闲对象落在 [page+64, page+4096) 内且按 object_size 对齐；canary 恒等于 0x5A5AA5A5C3C3DEDE。

[GUARANTEE]
必须实现：slub_basic_check、slub_size_class_check、slub_slab_grow_check、slub_free_reuse_check、slub_leak_check、slub_guard_check、slub_mixed_check、slub_oom_check、slub_cache_reuse_check 九个 static 函数，以及 void slub_check(void)。
允许：增加 static 辅助函数（结构自检、伪随机数发生器）。
不得：修改被测实现的语义来让测试通过；不得为了让用例通过而跳过断言；不得假设第一层一定分配成功。
必须保证：slub_check() 返回时 slub_nr_slabs() == 0 且 slub_nr_inuse_objects() == 0，页数与进入测试时相同。

[SPECIFICATION]
Pre-Condition：slub_init() 已执行；第一层页分配器可用；当前没有任何 slab。
Post-Condition：全部断言通过，输出九个用例结论与一行总结，分配与释放的页数平衡。

必须覆盖的用例：
Case 1（基本分配回收）：取最小档连续分配 3 个对象——非空、互不相同、落在同一页内、地址差为 8 的整数倍、slab 数为 1、已用对象数为 3；全部释放后 slab 数归零、页数还原。
Case 2（档位选择）：对九个档位各分配一次——对象地址按 8 字节对齐，且档位 <= 64 时还必须按自身大小对齐；全部释放后页数还原。另申请 2049 字节确认走直通路径。
Case 3（slab 扩容）：在 64 字节档位连续分配 64 个对象（一个 slab 只有 63 个）——slab 数必须恰好变成 2、已用对象数为 64、页数减少 2；全部释放后页数还原。
Case 4（释放后重用）：分配一个 32 字节对象 → 释放 → 再分配同尺寸——必须返回完全相同的地址（LIFO 的直接证据）。
Case 5（泄漏检查）：六个尺寸（含 3000 字节的直通对象）各分配三次后全部逆序释放——slab 数归零、已用对象数归零、页数与开始时完全相等。
Case 6（越界防护）：在 64 字节档位分配相邻两个对象——写满低地址对象的 64 字节不得改变高地址对象的内容；同时元数据 canary 必须完好。
Case 7（混合负载）：用伪随机数在 200 步内随机分配/释放六种尺寸（8/100/600/2049/5000/12）——每一步都检查结构不变量，且"已用对象数"必须等于现场 slab 对象数；结束时页数完全还原。
Case 8（非法与超大请求）：slub_alloc(0) 与 slub_alloc(1 TiB) 都必须返回 NULL，且页数与计数不变。
Case 9（cache 重建）：slub_init() 再次初始化——slab 数与对象总数归零；随后分配 2048 字节恰好得到 1 个 slab、1 个对象，释放后归零。

Requirements：

- 每个用例首尾都要对账：第一层页数、slab 数、已用对象数三项必须与用例前一致。
- 断言里不得出现具体地址常量；所有比较都依赖相对关系与不变量。
- 结构自检必须检查：canary、空闲对象地址区间、按 size 对齐、"freelist 长度 + inuse == 每页对象数"，并检测空闲链成环（用计数器上界截断）。
- 直通路径的对象不计入 cache 的已用对象数，测试统计时必须分开计数，避免用错误的计数口径制造假阳性。
```
