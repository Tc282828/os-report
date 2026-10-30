# 实验报告

## 实验基本信息

| 项目         | 内容                                           |
| ------------ | ---------------------------------------------- |
| **实验名称** | Lab 2：物理内存和页表                          |
| **小组成员** | 2514231 李文浩、2511203 严宗贺、2510582 田城昊 |
| **完成日期** | 2026-10-25                                     |

### 小组分工

| 成员               | 负责的练习/模块                                                                                                    |
| ------------------ | ------------------------------------------------------------------------------------------------------------------ |
| `[2510582 田城昊]` | 功能模块一任务一：物理内存探测与页描述符初始化；扩展 Challenge：硬件可用物理内存范围的获取方法                     |
| `[2514231 李文浩]` | 功能模块二：物理内存分配功能 任务一：理解first-fit 连续物理内存分配算法 任务二：实现 Best-Fit 连续物理内存分配算法 |
| `[2511203 严宗贺]` | 功能模块二任务三：Buddy System；扩展 Challenge：SLUB；Buddy/SLUB 测试用例与实现边界；三、实验整体逻辑分析（模块二与观察视角）                       |

---

## 一、实验目的

本实验的主要目的是：

1. 理解 RISC-V 内核从设备树获得物理内存布局的方法，掌握物理地址、内核虚拟地址、物理页号和 `struct Page *` 之间的关系。
2. 掌握以页为单位管理物理内存的方法，正确排除 OpenSBI、内核镜像、页表和页描述符数组占用的空间。
3. 理解 `page_init()` 如何把安全的完整空闲页注册给统一的物理内存管理器接口。
4. 理解页面分配算法的实现方式：在 First-Fit 的基础上实现 Best-Fit，并独立设计 Buddy System 的块组织方式、分裂与伙伴合并过程。
5. 按照 Lab0.5 的 `[PROMPT] / [RELY] / [GUARANTEE] / [SPECIFICATION]` 结构设计提示词，使用 AI 辅助实现、审查和验证内核功能。

---

## 二、实验环境

| 项目        | 配置                                          |
| ----------- | --------------------------------------------- |
| 宿主系统    | Windows + WSL Ubuntu                          |
| 源码编辑    | VS Code / Codex 桌面应用                      |
| AI 编程工具 | Codex                                         |
| 底层模型    | OpenAI GPT-5 系列（以客户端实际显示版本为准） |
| 交叉编译器  | `riscv64-unknown-elf-gcc 10.2.0`              |
| 模拟器      | QEMU 4.1.1，RISC-V `virt` 平台                |
| 固件        | OpenSBI                                       |
| 构建工具    | GNU Make                                      |

AI 工具使用情况：

| 成员               | AI 编程工具      | 底层模型           | 备注                                                   |
| ------------------ | ---------------- | ------------------ | ------------------------------------------------------ |
| `[2510582-田城昊]` | Codex 桌面应用   | OpenAI GPT-5 系列  | 用于阅读实验文档、设计提示词、实现审查和环境排错       |
| `[2514231-李文浩]` | opencode         | DeepSeek v4.1flash | 用于代码讲解、提示词设计与实现复核以及代码功能补全     |
| `[2511203-严宗贺]` | DeepSeek Harness | DeepSeek-V4-Flash  | 用于整体逻辑梳理、提示词编写、Buddy 模块实现与自测设计 |

本实验实际使用的关键路径如下：

```bash
export LAB2_DIR='/mnt/d/桌面文档/一些课程的论文/大二上/oslab/lab2_new'
export RISCV_PREFIX='/home/lenovo/tools/Risc-V/riscv64-unknown-elf-toolchain-10.2.0-2020.12.8-x86_64-linux-ubuntu14/bin/riscv64-unknown-elf-'
export QEMU_411='/home/lenovo/.local/qemu-4.1.1/bin/qemu-system-riscv64'
export QEMU_411_DEBUG='/home/lenovo/src/qemu-4.1.1/build/riscv64-softmmu/qemu-system-riscv64'
```

---

## 三、实验整体逻辑分析

### 3.1 本章节的逻辑主线

本报告覆盖 Lab2 的两个功能模块，它们回答的是同一个问题的前后两半。

模块一回答"内存从哪里来、哪些能用"：内核启动后怎样知道物理内存在哪里，怎样为每个物理页建立软件描述，以及怎样保证固件、内核和元数据占用的页面不会被当成空闲页。处理完成后，剩余完整页面通过统一接口交给当前物理内存管理器。

模块二回答"能用的页怎样按请求切分和回收"：既然管理器只拿到"从某个物理页开始、连续若干个完整页"这一种原料，它就必须自己维护空闲块的集合，并决定分配时挑哪一块、释放时怎样把碎块重新拼回大块。First-Fit、Best-Fit、Buddy System 三种实现的差别，全部落在这两个决策上——选块策略与合并策略，而不是页描述符本身。

整体调用链为：

```text
pmm_init()
  -> init_pmm_manager()                     选择并初始化分配策略（default / best_fit / buddy）
  -> page_init()
       -> get_memory_base()/get_memory_size() 从 DTB 取得 RAM 范围
       -> 初始化 pages[]                    建立并保留全部页描述符
       -> 计算 free_start/free_end          排除已占用和非完整页面
       -> init_memmap()                     把安全的完整空闲页登记给管理器
            -> pmm_manager->init_memmap()   按各算法自己的结构组织空闲块
  -> check_alloc_page()
       -> pmm_manager->check()              各分配器自带的正确性测试
运行期
  -> alloc_pages(n)  -> pmm_manager->alloc_pages(n)   按策略挑块、必要时切分
  -> free_pages(p,n) -> pmm_manager->free_pages(p,n)  按策略插回、必要时合并
```

### 3.2 功能的逐步实现

1. 首先通过 `get_memory_base()` 和 `get_memory_size()` 读取 DTB 解析结果，并检查范围是否有效。
2. 接着设置 `va_pa_offset`、`npage` 和 `pages`，为 `[nbase, npage)` 中的每个物理页建立描述符。
3. 然后把所有页面先标记为 Reserved，计算描述符数组结束后的页对齐物理地址，避免 OpenSBI、内核和描述符数组进入空闲区。
4. 最后仅在区间非空时调用 `init_memmap()`，注册可用完整页，并通过 QEMU 和课程注入测试验证结果。
5. 在 First-Fit 的完整实现上只改动分配时的选择策略——把"第一个装得下的块"换成"装得下且最小的块"，得到 Best-Fit；因为两者共用同一套"按地址有序 + 释放即合并"的骨架，这个改动可以做到只换一个函数体。
6. 由于 `pmm_manager` 一次只能指向一个实现，而课程评测的 `[Test 2]` 只认 `best_fit_pmm_manager`，所以 Best-Fit 完成后要把 `init_pmm_manager()` 切过去；Buddy 只能通过一个默认关闭的宏临时启用，验证完必须切回，否则会丢掉这 10 分。
7. 单独实现 Buddy System：块大小被限制为 2 的幂，空闲块按阶分链表组织，分配时二分、释放时按"页号异或块大小"找伙伴合并；它不参与课程评分，因此测试必须自己设计与实现，并挂在 `pmm_manager->check` 上随内核启动执行。
8. 最后统一验证：`make qemu` 观察初始化输出与各分配器自测结论，`make grade` 确认注入测试的两项检查点为 PASS。

---

## 四、实验内容与实现

### 功能模块一：物理内存初始化功能

**负责人：** `[2510582 田城昊]`

#### 模块功能描述

**需要实现/修改的函数：**

```c
static void page_init(void);    // kern/mm/pmm.c
```

`page_init()` 根据设备树提供的物理内存基址和大小，设置虚实地址偏移、物理内存末端页号以及页描述符数组。它先将全部受管页面标记为保留，然后计算描述符数组结束后的第一个完整物理页，只把该位置到物理内存末端之间的完整页注册给当前 `pmm_manager`。

本次默认 128 MiB QEMU 配置的实测内存布局如下；空闲区起点由内核和描述符数组大小计算得到，重新链接后可能变化：

```text
0x80000000  +-------------------------------+
            | OpenSBI                       |
0x80200000  +-------------------------------+
            | uCore 内核、栈和启动页表       |
kernel end  +-------------------------------+
            | 对齐空隙                      |
pages       +-------------------------------+
            | struct Page 描述符数组        |
0x80347000  +-------------------------------+
            | 可供 PMM 分配的完整物理页     |
0x88000000  +-------------------------------+
```

关键全局状态的含义如下：

| 变量            | 含义                                     |
| --------------- | ---------------------------------------- |
| `va_pa_offset`  | 内核高半虚拟地址和物理地址之间的固定偏移 |
| `npage`         | 设备树报告的物理内存末端对应的物理页号   |
| `nbase`         | DRAM 起始地址对应的物理页号              |
| `pages`         | `struct Page` 描述符数组的内核虚拟地址   |
| `npage - nbase` | 本实验真正需要建立描述符的物理页数量     |

#### 实现步骤与边界

1. 读取 DTB 解析结果，检查基址、长度、加法溢出及本平台的 `DRAM_BASE` 约定。
2. 设置 `va_pa_offset = PHYSICAL_MEMORY_OFFSET`，以 `PPN(mem_base + mem_size)` 得到末端页号 `npage`。实际描述符数量为 `npage - nbase`，不是直接分配 `npage` 个描述符。
3. 将链接符号 `end` 向上按 `PGSIZE` 对齐，得到描述符数组 `pages` 的内核虚拟地址；逐项清零状态并设置 Reserved。
4. 对 `pages + (npage - nbase)` 先用 `PADDR` 转为物理地址，再向上对齐得到 `free_start`；对物理内存末端向下对齐得到 `free_end`。
5. 仅在 `free_start < free_end` 时调用 `init_memmap(pa2page(free_start), (free_end - free_start) / PGSIZE)`，由管理器登记空闲页。

默认布局中 `npage = 0x88000`、`nbase = 0x80000`，所以共描述 `0x8000 = 32768` 个物理页。`0x80347000` 是本次构建的计算结果，代码没有硬编码该起点。

**实现范围：** 当前代码通过了本次默认 128 MiB 配置的测试；它固定要求 RAM 起点为 `DRAM_BASE`，尚未把 DTB 本体及设备树保留区从空闲集合中单独扣除，也未实现多段 RAM 管理。对超过框架映射上限的内存以及描述符数组本身容纳不下的极端小内存，还应补充映射上界和元数据容量检查。因此这里不宣称任意内存配置都已验证通过。

#### 最终提示词

以下提示词按照 Lab0.5 规定的四段式结构整理。本功能在建立实验记录前已完成，因此这是基于最终正确实现复盘得到的提示词；未将其伪装成最初输入。

```markdown
[PROMPT]
任务：修改 kern/mm/pmm.c，实现 page_init() 的物理内存探测、Page 描述符初始化和空闲物理页注册。
操作要求：必须修改项目中的真实文件；保留现有 pmm_init()、测试逻辑和 lab2 标记；不要修改公开接口，也不要硬编码某次运行得到的 free_start。
输出要求：仅完成该功能所需的代码；以工程内真实宏、数据结构和函数为准，不引入无关功能。

[RELY]

- DTB 接口：uint64_t get_memory_base(void)、uint64_t get_memory_size(void)。
- 页面大小与换算：PGSIZE、PPN、ROUNDUP、ROUNDDOWN。
- 地址关系：DRAM_BASE、PHYSICAL_MEMORY_OFFSET、PADDR。
- 全局状态：struct Page \*pages、size_t npage、const size_t nbase、uint64_t va_pa_offset。
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

- 严格区分虚拟地址、物理地址、物理页号和 Page \*。
- 起始地址向上对齐，物理内存末端向下对齐。
- 内核镜像与 Page 描述符数组占用的页面必须保持 Reserved。
```

#### 实现迭代过程

本模块的功能逻辑一次实现后通过了课程注入测试；环境验证经历了两轮。

##### 第一次迭代

**遇到的问题：**

- 临时使用 `riscv64-linux-gnu-gcc` 构建时，OpenSBI 能正常启动，但内核没有任何输出。
- 反汇编发现 `entry.S` 中的 `la` 通过 GOT/PIC 形式获取高半虚拟地址，而此时分页尚未完成，导致入口阶段异常。

**问题解决策略：**

- 对比 Lab0/0.5 环境和课程文档，确认本实验需要裸机 `riscv64-unknown-elf` 工具链。
- 改用已安装的 `riscv64-unknown-elf-gcc 10.2.0`，保持 QEMU 4.1.1 不变，重新清理并构建。

##### 第二次迭代

**最终结果：**

- ✅ 使用 QEMU 4.1.1 正常启动。
- ✅ 正确输出设备树物理内存范围和最终空闲区间。
- ✅ `page_init()` 的 8 项注入检查全部通过。
- ✅ `make grade` 总分为 `20/20`。

**关键点总结：**

1. `pages` 本身也占用物理内存，因此必须把描述符数组之后的位置作为空闲区起点。
2. 所有页面先 Reserved，再仅释放确认安全的完整页面，可以避免内核占用页被错误分配。
3. `mem_base == DRAM_BASE` 是本实验 QEMU `virt` 平台约束，不应推广为所有真实硬件的通用假设。

---

### Challenge：硬件的可用物理内存范围的获取方法

**负责人：** `[2510582 田城昊]`

操作系统通常不会通过盲目读写未知地址来试探内存，而是在启动阶段接收固件或 Bootloader 已经枚举好的内存描述。

在本实验的 RISC-V 启动流程中，QEMU 构造设备树，OpenSBI 启动内核时将 Hart ID 放入 `a0`，将 DTB 的物理地址放入 `a1`。`kern_entry` 保存 `a1` 后，`dtb_init()` 将 DTB 物理地址转换为内核虚拟地址，解析 `memory` 节点的 `reg` 属性，得到物理内存基址和大小，最后由 `page_init()` 查询并处理。

```text
QEMU 生成 DTB
  -> OpenSBI 在 a1 中传入 DTB 物理地址
  -> kern_entry 保存为 boot_dtb
  -> dtb_init() 解析 memory/reg
  -> get_memory_base()/get_memory_size()
  -> page_init() 排除已占用区域
  -> init_memmap() 注册可用完整页
```

真实系统需要在固件给出的 RAM 描述基础上继续排除下列区域：

- 固件和 Bootloader 占用的区域；
- 内核镜像、内核栈和启动页表；
- DTB 本体、FDT memory reservation block 和 `/reserved-memory`；
- `struct Page` 等内核元数据；
- 设备 MMIO 窗口、坏内存和平台规定不可使用的区域。

经过集合扣除、地址合法性检查和页边界对齐后，剩余区间才是真正可交给物理内存管理器的空闲页。若内存由多个不连续 RAM bank 组成，内核还应分别注册多个可用区间，而不能只保存一个 `[base, end)`。

其他平台也采用类似思想：传统 x86 可通过 BIOS E820 获得内存映射，UEFI 系统可在退出 Boot Services 前调用 `GetMemoryMap()`。如果固件完全不提供内存描述，则只能读取平台内存控制器寄存器或采用平台专用探测方法；直接对未知地址读写可能误触 MMIO，具有安全风险，不应作为首选方案。

本实验的 `dtb.c` 是教学简化版，只提取第一个 `memory` 节点中的一组 `reg`。因此它足以支持固定的 QEMU `virt` 环境，但尚未覆盖多段 RAM 和全部保留区，这正是实验模型与真实操作系统之间的主要差异。

---

### 功能模块二：物理内存分配功能

**负责人：** `[2514231-李文浩]`

#### 任务一：理解first-fit 连续物理内存分配算法（思考题）

初始化空闲页：是default_init，default_init_memmap两个函数实现的，第一个函数保证置空链表和nr_free计数为零，第二个函数负责登记内存为空闲块，每页的标记也会被改变，property从1变0，首页property变为n，每一块只有一个节点，并且nr_free计数更新

分配连续物理页：default_alloc_pages(n)负责这个功能，这是first的体现，也是与bestfit功能的主要区别。当n > nr_free时，可用页总数都不够就不需要再寻找了。总数够时，就从表头往下扫（无论增删节点都保证链表按内存块首物理地址升序排列），依赖这个有序性，找到第一个满足property >= n的块，然后记住这个块的前驱，将他截取下来，同时如果如果块比 n 大，就在page + n的位置切出块剩余部分，作为新的块首页插回原来的位置，保证链表仍然有序。此外更新nr_free。

回收页面并合并相邻空闲块：default_free_pages(base, n)负责此功能。运行时先检查这些页不是保留页、也不在空闲链表上，避免释放内核页或重复释放；然后把标志和引用计数清零，首页写property = n，更新nr_free，再按地址插回链表。

每页都有一个记录该页的性质的小内存，包括ref引用计数；flags状态位图，它的值代表着该页的状态；property；链表节点，节点有next和prev两个指针。

改进方法：1.因为每次查找块都是从低地址开始，会导致低地址的块越来越碎，高地址的空闲块利用率不大。可以试试随机从低地址或高地址开始查找，或者使用bestfit分配方法。2.我觉得操作系统忙来忙去有一个统一的目标，就是提高资源利用率。这个单链表的话不适用于并发，不适配多核cpu。（猜测）3.可以加点其他的观测值，如最大property等，方便我们确定内存分配失败的原因。

#### 任务二：实现 Best-Fit 连续物理内存分配算法（设计提示词借助 AI 完成）

firstfit分配方法与bestfit分配方法的唯一区别在分配时的选择策略：前者取链表中第一个足够大的块，依赖地址有序，命中即停，而后者遍历整条链表，取满足需求的最小块，因此能保留大块，但每次分配都要扫完链表。

提示词：
[PROMPT]

给lab2补充一个bestfit物理内存分配器，写在新文件 kern/mm/best_fit_pmm.c 里。

模仿kern/mm/default*pmm.c ，其中初始化、登记空闲页、分配、释放、查询剩余页数的五个函数都保留，函数名加 best_fit*前缀。文件里原有的两个测试函数不要动，也不要复制。
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
五个静态函数，名字加 best*fit* 前缀（初始化、登记、分配、释放、查询）；
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

best-fit方法问题与改进：1.很明显，用时会比firstfit方法长。可以根据块大小对链表进行分档，如1，2，4，8......,每档一个链表。2.实际应用时1页的需求多么？如果不多的话那为了避免剩下大量零碎的1页空闲页，当property-n=1时就全给了吧。3.切分时从尾页向前切割，也就是首页不动，可以减少节点的改变，更能保证节点的有序性。

#### 任务三：buddy system（伙伴系统）分配算法（设计提示词借助 AI 完成）

**负责人：** `[2511203-严宗贺]`

##### 模块功能描述

**新增与修改的文件：**

```c
// 新增 kern/mm/buddy_system_pmm.c、kern/mm/buddy_system_pmm.h
static void          buddy_init(void);
static void          buddy_init_memmap(struct Page *base, size_t n);
static struct Page  *buddy_alloc_pages(size_t n);
static void          buddy_free_pages(struct Page *base, size_t n);
static size_t        buddy_nr_free_pages(void);
static void          buddy_check(void);          // 本任务第二份 prompt 生成的测试
const struct pmm_manager buddy_system_pmm_manager;

// 修改 kern/mm/pmm.c
#define LAB2_VERIFY_BUDDY_PMM 0                  // 仅供单独验证 Buddy 的开关，提交状态必须为 0
static void init_pmm_manager(void);              // 按开关选择 best_fit 或 buddy
```

任务一、任务二的两个分配器有一个共同点：空闲块的**大小可以是任意页数**，块与块的边界完全靠块头页的 `property` 记录。Buddy System 把这一点改掉了——**每个空闲块的大小都必须是 2 的幂**。这个限制换来了一个很强的性质：块的首地址必然按自身大小对齐，于是"伙伴"可以通过页号或描述符下标异或块页数计算，不需要额外记录谁和谁相邻；代价是内部碎片，申请 3 页只能得到 4 页的块。

内部状态设计如下：

```c
#define BUDDY_MAX_ORDER 19          /* 单块最大 2^19 页 = 2 GiB，覆盖本实验的 128 MiB */

typedef struct {
    list_entry_t free_list[BUDDY_MAX_ORDER + 1];   /* 每一阶一条空闲块链表 */
    unsigned int nr_free;                          /* 空闲页总数 */
} buddy_free_area_t;
```

| 信息         | 存放位置                   | 含义                                                 |
| ------------ | -------------------------- | ---------------------------------------------------- |
| 块大小       | 块头页的 `property`        | 该块共 `2^order` 页，恒为 2 的幂                     |
| 是否空闲块头 | 块头页的 `PageProperty` 位 | 置位表示"挂在某阶链表上的空闲块首页"；分配出去后清除 |
| 属于哪一阶   | 由块大小取对数得到         | 决定挂在 `free_list[order]` 上                       |
| 成员关系     | `page_link`                | 挂到对应阶的链表                                     |

为什么 `BUDDY_MAX_ORDER` 取 19：受管区间从 `DRAM_BASE` 起算，`nbase = DRAM_BASE / PGSIZE = 0x80000 = 2^19` 页。只要块不超过 `2^19` 页，"描述符下标 `page - pages`"和"物理页号 `page2ppn(page)`"就同时按块大小对齐，伙伴计算可以直接用下标异或；超过这个阶，两种视角的对齐关系就不再一致。QEMU `virt` 默认 128 MiB，扣除内核镜像和描述符数组后空闲区约 3.2 万页，实际最大块只到 14 阶。

**分配（分裂）**：把请求页数向上取整到 `2^k`，从第 k 阶开始向上找第一个非空链表，取出其中的块，再逐级二分：一次分裂把块拆成两个低一阶的块，低的那半继续切，高的那半挂到对应阶的空闲链表上，直到剩下的大小正好是 `2^k`，把它作为结果返回。因为始终保留低位那一半，返回的块首地址与最初取出的块首地址相同，天然满足对齐要求。

**释放（合并）**：块头页的页号异或其块大小，就是伙伴块的页号。若伙伴块同阶、正挂在空闲链表上且未被保留，就把两块合并、阶数加一，然后继续尝试与更高阶的伙伴合并，直到伙伴不在空闲状态为止，最后把合并结果挂回对应阶的链表。

**记账约定（本任务最容易出错的点）**：框架给 `free_pages(base, n)` 的 `n` 是**调用者当初请求的页数**，而 Buddy 实际给出的块可能更大（请求 3 页得到 4 页的块）。如果释放时按 `n` 反推阶数，就会得出 2 页的块，与真实块边界错位，后续合并会算到错误的伙伴页上。因此分配时在块头页保留 `property = 2^k`（同时清除 `PageProperty`，用这个位区分"空闲块头"和"已分配块头"），释放时以块头记录的真实块大小为准，规则是 `size = max(块头 property, 向上取整到 2 的幂的 n)`。

##### 设计提示词

按课程要求，本任务提交两份提示词：第一份从零生成 Buddy 模块并接入内核，第二份针对这份实现设计测试函数。

**第一份提示词（生成模块并接入内核）：**

```markdown
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
- 工程约定：kern/mm/\*.c 会被 Makefile 自动编译；init_pmm_manager() 当前挂 best_fit_pmm_manager；课程评测的 [Test 2] 只认 best_fit_pmm_manager。

[GUARANTEE]
必须实现：上面六个静态函数，以及常量 const struct pmm_manager buddy_system_pmm_manager（名字不可改），六个成员分别指向这六个函数；头文件中声明该常量。
允许：在 kern/mm/buddy_system_pmm.c 内自行设计内部状态（按阶组织的空闲链表、计数器）与静态辅助函数。
不得修改：default_pmm.c、best_fit_pmm.c、pmm.h、memlayout.h、list.h、tools/\*。
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

**第二份提示词（针对该实现设计测试）：**

```markdown
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
不得修改：default_pmm.c、best_fit_pmm.c 及其中已有的测试、tools/\*。
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

##### 实现迭代过程

本模块的实现经历了两次迭代，两次都不是"写不出来"，而是"能跑但账不对"。

###### 第一次迭代

**遇到的问题：**

- 最初照搬 Best-Fit 的骨架，用一条按地址排序的空闲链表，分配时把块切成"请求的 n 页 + 剩余部分"两块。
- 这样得到的空闲块大小不再是 2 的幂，"伙伴"失去了定义，释放时只能退化成按地址找左右邻块合并，本质上变成了 First-Fit 的变体；更严重的是计数对不上：申请 3 页实际占用 4 页，但 `free_pages(base, 3)` 只回收 3 页，第 4 页永久泄漏，自测里"用例前后 nr_free 相等"这一条直接失败。
- 另外，`basic_check` 风格的自测只检查分配出的页面互不相同、ref 为 0，这类检查完全抓不到上面的泄漏。

**问题解决策略：**

- 明确 Buddy 的硬约束：块大小只能是 2 的幂，块首必须按自身大小对齐。
- 把内部状态从"一条链表"改成"每一阶一条链表"（`free_list[order]`），块大小记录在块头页的 `property` 中。
- 在提示词的 `[GUARANTEE]` 里补上"必须保证各阶空闲块页数之和等于 nr_free"，并让测试函数在每个用例前后做对账，而不是只看返回指针。

###### 第二次迭代

**遇到的问题：**

- 改成多阶链表后，分裂与合并的结构正确了，但释放仍然出错：只按调用者传入的 `n` 反推阶数，于是"申请 3 页、释放 3 页"因错误地向下取整被当成 2 页的块处理，块边界错位，下一次合并时算到了别人的伙伴页上，表现为链表里出现大小与阶不符的块，甚至 panic。
- 另一个隐蔽问题是合并时只判断了"伙伴页是否在链表上"，没有校验伙伴块的 `property` 是否等于当前阶，导致把不同阶的块当成伙伴。

**问题解决策略：**

- 在 `[SPECIFICATION]` 里把记账规则写死：分配出去的块头保留 `property = 2^k`、清除 `PageProperty`；释放时以块头记录的真实块大小为准，规则是 `size = max(块头 property, 向上取整到 2 的幂的 n)`。
- 合并条件补全为三个：伙伴块 `PageProperty` 置位、`property` 等于当前阶大小、且页号未越过受管区间；合并后用 `buddy_push_free_block()` 统一"设 property + 置位 + 挂链表"的动作，避免三处重复代码各自漏一步。
- 让 `buddy_sum_free_pages()` 在遍历时同时校验"块大小与所在阶一致""块首按自身大小对齐"，把结构性错误变成断言，而不是留到后面变成随机 panic。

**最终结果：**

- ✅ 编译通过，五个接口与管理器常量注册正确。
- ✅ 自带 5 项测试全部通过，输出 `[buddy test] all buddy checks passed`。
- ✅ 用宏开关切到 Buddy 后 `make qemu` 正常启动；切回 Best-Fit 后 `make grade` 仍为 `20/20`。

**关键点总结：**

1. Buddy 的"伙伴"只有在块大小为 2 的幂、且块首按自身大小对齐时才有意义；一旦允许任意大小的块，算法就退化。
2. `alloc_pages(n)` 的返回值可能比请求更大，因此"真实块大小"必须由分配方记录、由释放方读取，不能各自用 `n` 推算。
3. `PageProperty` 位在这里承担了第二个语义：区分"空闲块头"与"已分配块头"，而 `property` 字段在两种状态下都保存块大小。
4. 自带测试如果没有"对账"步骤，只能验证"能分配出去"，验证不了"没有泄漏"。

##### 测试覆盖与有效性说明

测试挂在 `pmm_manager->check` 上，随内核启动执行，共 5 个用例；每个用例前后都对账。

| 用例（实现函数） | 覆盖的核心行为 / 边界 | 为什么能有效检验实现 |
| --- | --- | --- |
| 基本分配回收（`buddy_basic_check`） | 单页分配与释放、页面不重复、ref 归零、精确计数 | 任何页数泄漏都会让 `nr_free` 与用例前不等，这是最基础的回归 |
| 分配粒度（`buddy_order_check`） | 分配粒度必须是 2 的幂、块首按自身大小对齐、块头记录真实大小 | 申请 3 页只能给 4 页块；能暴露"按 n 而不是按 2^k 记账"这一类错误 |
| 分裂与合并（`buddy_split_merge_check`） | 不分裂路径；把最大块切成两半再分别释放后应重新合并成整块 | 只有分裂与合并都正确，两个半块才能拼回原块并再次整块分配；这是对伙伴算法最直接的验证 |
| 越界请求（`buddy_limit_check`） | 请求大于空闲总量、大于单块上限时必须返回 NULL 且状态不变 | 检验边界判断，防止越界返回非法页或误改计数 |
| 耗尽路径（`buddy_exhaust_check`） | 以 16 MiB（4096 页）为单位分配直到没有合适块，再逆序全部释放 | 检验"找不到块"的分支与批量回收后的状态恢复，能暴露分裂过程中丢失的空闲块 |

5 个用例由 `buddy_check()` 依次调用，注册在 `buddy_system_pmm_manager.check` 上，`pmm_init()` 末尾的 `check_alloc_page()` 会触发它，因此只要内核启动就能看到自测结论。

对账函数 `buddy_sum_free_pages()` 遍历所有阶的空闲链表，逐个检查 `PageProperty` 置位、块大小与所在阶一致、块首按自身大小对齐，并累加页数，用来与 `nr_free` 比较。它把"结构不变量"变成了断言：一旦某次分裂或合并写错了标记或漏改了计数，测试会立刻 panic 并指出是哪条约束被破坏，而不是等到后续随机分配时出现难以定位的越界。

##### 问题回答：你的 Buddy 算法是否有进一步的改进空间？

1. 每次分配都要从请求阶向上扫描找第一个非空链表，最坏要扫 `BUDDY_MAX_ORDER` 条链表。可以用位图记录"哪些阶非空"，把查找降为常数级；也可以把阶的上界按实际内存裁剪，避免为不可能出现的阶浪费判断。
2. 块大小只能是 2 的幂，申请 3 页占 4 页，内部碎片在最坏情况下接近 50%。缓解办法是两层结构：底层用 Buddy 管理页块，上层再用 slab 一类的对象分配器管理小块，避免每次都按整块分配。
3. 伙伴判定用"描述符下标异或块大小"，依赖 `nbase` 是 2 的幂。真实硬件的 DRAM 起点不一定对齐到大页，此时要改用物理页号做对齐修正，否则伙伴会算到受管区间之外。
4. 合并只在释放时发生，频繁的分配/释放会产生大量链表操作。可以保留少量"每阶缓冲块"或采用延迟合并，把多次小操作合并成一次。
5. 目前 `init_memmap()` 只登记一段连续区间，没有处理多段 RAM、NUMA 节点或保留区；真实系统需要按区间分别登记，并在合并时保证不跨越区间边界。

##### 验证结果

先把 `kern/mm/pmm.c` 中的开关改为 `1`，重新编译并运行：

```c
#define LAB2_VERIFY_BUDDY_PMM 1
```

`make qemu` 的关键输出（空闲页数 = `(0x88000000 − 空闲区起点) / 4096`，会随内核镜像体积变化；本机实测 31929 页，此时内核 footprint 为 25 KB）：

```text
DTB init completed
(THU.CST) os is loading ...
Kernel executable memory footprint: 25KB
memory management: buddy_system_pmm_manager
physical memory: [0x0000000080000000, 0x0000000088000000)
page descriptors: 32768 pages at 0xffffffffc0207000
free physical memory: [0x0000000080347000, 0x0000000088000000)
[buddy test] buddy_system_pmm_manager self-test, free pages = 31929
  [buddy] single page alloc/free .......... OK
  [buddy] power-of-two granularity ........ OK
  [buddy] split & merge .................. OK
  [buddy] over-size request rejected ...... OK
  [buddy] bulk alloc/free until exhausted  OK
[buddy test] all buddy checks passed
check_alloc_page() succeeded!
```

验证完必须把开关改回 `0` 再提交，否则评测的 `[Test 2]` 会因为 `pmm_manager` 不是 `best_fit_pmm_manager` 而失败。

切回提交态后，本机用 `tools/grade_new_qemu.sh` 做了本地等价复现（`-kernel` 启动 + 调用与 `make grade` 同一份 `tools/inject.py`），确认"切到 Buddy 再切回来"没有破坏 Best-Fit：

```text
[Test 1] Page Init: PASS
[Test 2] Best-Fit: PASS
check page init: OK      (10/10)
check best fit: OK       (10/10)

Total Score: 20/20
```

![Buddy System 自测结果](./images/lab2-buddy-check-2511203.png)

*图 1：Buddy 自测（2026-10-29 本机实测，WSL2 Ubuntu + QEMU 10.2.1 + OpenSBI v1.8）。5 项测试全部通过，`free pages = 31929`，随后 `check_alloc_page() succeeded!`。此图拍摄时树上还没有 SLUB 模块，内核 footprint 为 25 KB；加入 SLUB 后内核增到 29 KB，同一台机器上该值变为 31928 页（见"挑战任务 SLUB"的验证结果），少掉的正好是内核多吃的一页。*

![切回 Best-Fit 后的本地等价评分](./images/lab2-grade-local.png)

*图 2：切回 Best-Fit 后的本地等价评分。该脚本以 `-kernel` 方式启动并复用 `tools/inject.py` 注入课程测试，输出与官方 `make grade` 相同的两个检查点与计分格式，但属于本地复现，非官方评分记录；官方评分仍需 QEMU 4.1.1 环境。*

---

#### 挑战任务：任意大小的内存单元 SLUB 分配算法（设计提示词借助 AI 完成）

**负责人：** `[2511203 严宗贺]`。本项对应指导书「扩展任务 Challenge：任意大小的内存单元 slub 分配算法」，属加分项，不参与课程自动评分。

##### 模块功能描述

**新增与修改的文件：**

```c
// 新增 kern/mm/slub_pmm.c、kern/mm/slub_pmm.h
void   slub_init(void);
void  *slub_alloc(size_t size);
void   slub_free(void *ptr);
size_t slub_nr_slabs(void);          // 当前 slab（页）总数，供测试对账
size_t slub_nr_total_objects(void);  // 所有 slab 能切出的对象总数
size_t slub_nr_inuse_objects(void);  // 已分配对象数
void   slub_check(void);             // 本任务第二份 prompt 生成的测试

// 修改 kern/init/init.c（三行，不动 pmm.c 与三个分配器）
#include <slub_pmm.h>
...
    pmm_init();                      // init physical memory management
    slub_init();                     // 第二层：在页分配器之上建立对象分配器
    slub_check();                    // SLUB 自测，随内核启动执行
```

指导书要求"第一层基于页大小的内存分配、第二层在第一层基础上实现任意大小的内存分配"。这两层的分工是：**第一层不自己管物理页**，只把 `alloc_pages()/free_pages()` 当成"按页租地"的接口；**第二层把租来的页切成等长对象**，用空闲对象链表摘取和回收对象；链头操作为 O(1)，查找可用 slab 和识别释放归属仍需扫描 slab 链。这样既复用了已经验证过的页分配器（Best-Fit 或 Buddy 都可以垫在下面），又避免了"每个小对象都占一整页"的浪费。

内部状态设计如下：

```c
#define SLUB_NUM_CLASSES  9                       /* 档位：8/16/…/2048 字节 */
#define SLUB_MAX_OBJECT   2048
#define SLUB_SLAB_PAGES   1                       /* 一个 slab 一页 */
#define SLUB_OBJ_OFFSET   64                      /* 页首 64 字节留给元数据 */

struct slub_slab {                                /* RV64 下为 40 字节，放在页首 64 字节保留区 */
    struct slub_slab *next;                       /* 同一 cache 的 slab 链 */
    void             *page;                       /* slab 首页虚拟地址 */
    unsigned int      class_index;                /* 属于哪个档位 */
    unsigned int      inuse;                      /* 已分配对象数 */
    void             *freelist;                   /* 空闲对象链（LIFO） */
    unsigned long     canary;                     /* 元数据哨兵 0x5A5AA5A5C3C3DEDE */
};

struct kmem_cache {                               /* 一个档位一个 cache */
    size_t       object_size;
    unsigned int objects_per_slab;                /* (4096-64)/object_size */
    unsigned int slab_count;
    unsigned int inuse;
    struct slub_slab *slabs;
};
```

九档对象容量（`(PGSIZE - 64) / object_size` 取整）：

| 对象大小 | 8 | 16 | 32 | 64 | 128 | 256 | 512 | 1024 | 2048 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 每页对象数 | 504 | 252 | 126 | 63 | 31 | 15 | 7 | 3 | 1 |

| 信息 | 存放位置 | 含义 |
| --- | --- | --- |
| 对象大小 | `slub_caches[i].object_size` | 由 `slub_class_of()` 向上找第一个 `>= size` 的档位得到 |
| 是否空闲 | 是否在 `slab->freelist` 链上 | 空闲对象前 8 字节（`sizeof(void *)`）存"下一个空闲对象地址"，因此最小档位必须 `>= 8` 字节 |
| 已分配数 | `slab->inuse`、`cache->inuse` | 两层计数，前者用于判断能否销毁 slab，后者用于统计接口 |
| slab 归属 | 地址区间 `[page+64, page+4096)` | 释放时扫描各 cache 的 slab 区间即可判断指针是谁的，不需要额外索引 |
| 大对象页数 | 直通路径页首 16 字节头 | `header[0] = magic`、`header[1] = 页数`，返回 `header+16` |

**分配**：`slub_class_of(size)` 找到档位 → 在该 cache 的 slab 链上找第一个 `freelist` 非空的 slab → 摘下链头对象、`freelist` 前移、两层 `inuse++`；若所有 slab 都满则向页级层要 1 页、把对象区从低到高切成等长对象压入 `freelist`（形成 LIFO，于是"刚释放的对象最先被复用"）。

**释放**：先按地址区间判断指针属于哪个 cache 的哪个 slab；命中则把对象头插回 `freelist`、两层 `inuse--`，**当 `inuse == 0` 时立刻销毁该 slab 并把页还给页级层**；若不落在任何 slab 内，则按直通路径处理：回退 16 字节读 magic 与页数，原样 `free_pages()`。

**记账约定（本模块最关键的取舍）**：`slub_free()` 只有指针、没有长度参数，因此"这次释放多少"不能由调用方声明——落在 slab 里就按该档位的对象回收，落在 slab 外就读页首头里的页数。这与 Buddy 必须"以块头 `property` 为准"是同一类问题：**分配方记录了真实尺寸，释放方就必须读它，而不能相信自己收到的参数**。空 slab 立即归还这一条，则把"有没有泄漏"变成了可以直接用 `nr_free_pages()` 比较的数字，而不是需要人工推演的不变量。

**代价**：档位向上取整造成内部碎片（申请 100 字节实占 128 字节，最坏接近 2 倍）；每页 64 字节元数据约占 1.6% 空间；一个 slab 只切出 1～3 个对象的大档位（1024/2048）空间利用率不高。指导书明确"时空复杂度不做要求，能通过测试即可"，因此这里优先选择**结构简单、状态可对账**，而不是追求利用率。

##### 设计提示词

按课程要求提交两份提示词：第一份从零生成 SLUB 模块并接入内核，第二份针对这份实现设计测试函数。

**第一份提示词（生成模块并接入内核）：**

```markdown
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

**第二份提示词（针对该实现设计测试）：**

```markdown
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

##### 实现迭代过程

本模块共经历三次迭代，问题都集中在"记账口径"和"回收边界"，而不是分配算法本身。

###### 第一次迭代：把元数据放在静态数组里

**遇到的问题：** 最初为每个 slab 的描述信息单独开了一个 `static struct slub_slab slabs[MAX_SLABS]` 数组，`max_slabs` 只能凭经验取，测试稍微放大就要改常量；而且"由对象地址反推它是哪个 slab"还得遍历数组比较区间，释放路径变长。

**修改：** 改成**元数据就放在 slab 首页的前 64 字节**，对象区从 `page + 64` 开始。这样"对象地址 → slab"只要按 4 KiB 页取下界即可命中元数据本身，没有容量上限，也不占额外页；同时补了一个编译期断言 `sizeof(struct slub_slab) <= 64`，防止以后加字段时悄悄侵占对象区。

**教训：** 元数据放哪里不是噪声，它决定了"释放时能不能只靠指针找到归属"。

###### 第二次迭代：slab 空了却不还页

**遇到的问题：** 第一版为了让"slab 可以复用"，在 `inuse == 0` 时只把 slab 留在链上、不把页还给页级层。结果 `slub_leak_check` 无法写——"全部释放后页数应该回到初值"这条最有力的断言失效了，泄漏只能靠人工推演，而泄漏恰恰是分配器最需要机器检查的性质。

**修改：** 改为 `inuse == 0` 时**立即销毁 slab 并 `free_pages()` 归还整页**。代价是"全空的 cache 会频繁地在销毁/重建之间摆动"，但在本实验规模下这点开销无关紧要；换来的是**每个用例都能用 `nr_free_pages()` 对账**，泄漏变成一行断言。

**教训：** 分配器的可测性来自它的记账方式，而不来自测试写得多细。把"能对账"当成设计目标，测试才有落点。

###### 第三次迭代：直通路径的头部与页数

**遇到的问题：** 最初对大对象按 `ceil(size / PGSIZE)` 取页并直接返回页首地址。两个问题：一是释放时只能拿到指针，"这块有几页"无从得知；二是如果只按 `size` 取页，一旦将来为了对齐或元数据在页首留头，就会少一页而越界写。

**修改：** 直通路径改为 `pages = ceil((size + 16) / PGSIZE)`，页首 16 字节写 `magic` 与页数，返回 `header + 16`；释放时回退 16 字节，先校验 `magic` 再按头里的页数归还，并把 magic 清零。这样"释放不需要长度参数"这件事在两条路径上都成立（slab 靠地址区间，直通靠页首头）。

**教训：** "释放接口不带长度"是 SLUB 这类分配器的常见设计，但它的前提是**分配方一定把长度写在了分配方自己拿得到的地方**。

###### 补丁：混合用例的计数口径

第一版的 `slub_mixed_check` 用一个 `filled` 同时统计 slab 对象与直通大对象，再与 `slub_nr_inuse_objects()` 比较，结果必然不等——因为直通对象不在任何 cache 里。后来改成每个槽位记住自己的尺寸，并区分 `small_filled`（走 slab 的对象）与总数；同时把"freelist 长度 + inuse == objects_per_slab"提升为结构自检的一部分，任何一次记账错误都会在下一次自检时被抓住，而不是等到最后算总账。

##### 测试覆盖与有效性说明

测试写在 `kern/mm/slub_pmm.c` 内，由 `slub_check()` 依次调用，随内核启动执行（`kern_init()` 在 `pmm_init()` 之后调用）。

| 用例（实现函数） | 覆盖的核心行为 / 边界 | 为什么能有效检验实现 |
| --- | --- | --- |
| 基本分配回收（`slub_basic_check`） | 最小档 3 个对象同页、地址不重叠、释放后 slab 销毁、页数还原 | 同时验证"同页切分"与"空 slab 回收"两件事；少还一页立即暴露 |
| 档位选择（`slub_size_class_check`） | 九个档位各分配一次、档位内按自身大小对齐、2049 走直通 | 检验向上取整的档位查找是否正确，以及档位上限与直通路径的切换点 |
| slab 扩容（`slub_slab_grow_check`） | 64 字节档连续 64 个对象 → 恰好 2 个 slab、页数减少 2 | 检验"单 slab 用尽时新建"而不是越过页末端继续切分 |
| 释放后重用（`slub_free_reuse_check`） | 分配→释放→再分配同尺寸，地址必须相同 | 直接检验 freelist 的 LIFO 语义；链头摘错会返回别的地址 |
| 泄漏检查（`slub_leak_check`） | 六个尺寸（含 3000 字节直通）×3 次后逆序全释放 | 混合路径的泄漏与计数错乱；逆序释放还能覆盖不同回收顺序 |
| 越界防护（`slub_guard_check`） | 相邻两对象，写满低地址者后高地址者内容不变 | 对象边界算错（例如按 `size` 而非 `object_size` 切）会立刻表现为越界写 |
| 混合负载（`slub_mixed_check`） | 200 步随机分配/释放六种尺寸，每步校验不变量 | 长期运行下的计数漂移与链表损坏，静态用例覆盖不到 |
| 非法与超大请求（`slub_oom_check`） | `size = 0`、`size = 1 TiB` 都必须返回 NULL 且状态不变 | 失败路径不得改动任何状态；避免页级层被越界请求打穿 |
| cache 重建（`slub_cache_reuse_check`） | `slub_init()` 再跑一次，计数归零、随后 2048 字节恰好 1 slab/1 对象 | 全局状态复位；2048 档"每页只切一个"的边界 |

九个用例之外还有一个**结构自检**函数（`slub_structure_ok()`），检查：canary 是否完好、空闲对象是否都落在对象区内并按 `object_size` 对齐、"freelist 长度 + inuse"是否恰好等于 `objects_per_slab`、空闲链是否成环。它在多个用例中被反复调用，作用是"在错误的下一步之前停住"，而不是等最终计数对不上再回溯。

##### 实现边界清单

| 编号 | 边界情况 | 实现中的处理方式 | 覆盖用例 |
| --- | --- | --- | --- |
| S1 | `size = 0` | `slub_alloc` 最先判 `size == 0` 返回 NULL，不进入档位查找 | `slub_oom_check` |
| S2 | 大小恰为档位值（8/16/…/2048） | `slub_class_of` 返回第一个 `size <= 档位` 的下标，恰好命中本档，不跳到下一档 | `slub_size_class_check` |
| S3 | 大小 2048 与 2049 | 2048 归对象层（每页 1 个对象）；2049 超过 `SLUB_MAX_OBJECT`，走直通路径取 1 页 | `slub_size_class_check` |
| S4 | 直通路径的页数 | `pages = ceil((size + 16) / PGSIZE)`：2049 → 1 页，5000 → 2 页 | `slub_leak_check`、`slub_mixed_check` |
| S5 | 单 slab 对象用尽 | `slab->freelist == NULL` 时向页级层新建 slab，绝不超过 `page + 4096` 继续切分 | `slub_slab_grow_check` |
| S6 | 释放 slab 内最后一个对象 | `inuse == 0` → 从 cache 链摘除 → `free_pages()` 归还整页 | `slub_basic_check`、`slub_leak_check` |
| S7 | 释放后立即重新申请 | freelist 头插、分配时摘头，因此必然先返回刚释放的对象 | `slub_free_reuse_check` |
| S8 | 对象边界 | 写满合法对象范围后，检查相邻对象及元数据未改变；不提供通用越界写检测 | `slub_guard_check` |
| S9 | `slub_free(NULL)` | 直接返回；本次在 `slub_oom_check` 中增加显式调用 | `slub_oom_check` |
| S10 | 非法指针或重复释放 | 不受支持；内部断言只覆盖部分情况，任意指针可能先触发访存异常，同一 slab 内重复释放可能破坏空闲链 | 无负向测试，调用方必须保证指针合法且只释放一次 |
| S11 | 页级层拒绝分配（1 TiB） | `alloc_pages` 返回 NULL → `slub_alloc` 返回 NULL，页数与计数不变 | `slub_oom_check` |
| S12 | cache 重新初始化 | `slub_init()` 重置九个 cache 的链与计数；此前必须没有未释放对象 | `slub_cache_reuse_check` |
| S13 | 混合尺寸、随机顺序释放 | 各 cache 计数独立，`slub_nr_inuse_objects()` 只统计 slab 对象；直通对象单独统计 | `slub_mixed_check` |
| S14 | 结构检查 | `slub_structure_ok()` 检查 canary、空闲对象区间、相对档位对齐、空闲数与 inuse 之和，并限制遍历长度 | 由部分用例及测试末尾显式调用，不是每次操作都会检查 |

边界表同时列出了已覆盖行为和未支持情况。尤其非法指针、重复释放、并发和完整耗尽场景不能仅凭正常路径自测认定安全。

##### 验证结果

由于本机没有课程提供的 C 交叉工具链时也需要先排除逻辑错误，先用与 `slub_pmm.c` 逐行等价的 Python 复现（`tools/verify_slub_logic.py`）跑同一批断言，其中包括 3000 步随机压力测试：

```text
== SLUB 逻辑等价验证（Python 逐行复现 slub_pmm.c）==
物理内存 32768 页 / 128 MiB，初始空闲 32768 页
[ OK ] basic alloc/free              三个同档对象同页、释放后页数还原
[ OK ] size class selection          九个档位都能分配，地址按档位对齐
[ OK ] slab grow on demand           一个 slab 装不下时自动开第二个，并各自回收
[ OK ] free then reuse               释放后立即重用同一对象
[ OK ] no leak after mixed free      跨档位含直通路径，全部释放后零泄漏
[ OK ] neighbour guard               写满一个对象不影响相邻对象，元数据 sentinel 完好
[ OK ] mixed random workload         随机大小/随机顺序的 200 步混合负载，最终完全归还
[ OK ] invalid & oversized request   0 字节与超出物理内存的请求返回 NULL 且不改变页数
[ OK ] cache re-init                 cache 重新初始化后计数清零、仍可正常分配
[ OK ] stress(3000 steps)            3000 步随机操作后不变量与页数均正确
========================================================
全部 10 个用例通过；最终空闲 32768 页（初始 32768 页）
分配 1253 页 / 释放 1253 页 / 失败 1 次
```

随后在实机上编译运行（SLUB 自测不需要改任何宏，`kern_init()` 每次启动都会调用）：

```bash
sh tools/slub_check.sh        # make clean + make -j2 + 以 -kernel 方式启动 QEMU
```

`make` 一次通过（`gcc ... kern/mm/slub_pmm.c`、`ld bin/kernel`、`objcopy ... bin/ucore.img`），QEMU 的关键输出如下（`slub_check()` 紧跟在 `pmm_init()` 之后执行，所以它排在 `check_alloc_page()` 之后）：

```text
DTB init completed
(THU.CST) os is loading ...
Kernel executable memory footprint: 29KB
memory management: best_fit_pmm_manager
physical memory: [0x0000000080000000, 0x0000000088000000)
page descriptors: 32768 pages at 0xffffffffc0208000
free physical memory: [0x0000000080348000, 0x0000000088000000)
check_alloc_page() succeeded!
satp virtual address: 0xffffffffc0206000
satp physical address: 0x0000000080206000
[slub test] slub_pmm self-test, free pages = 31928
[slub] basic alloc/free ............ OK
[slub] size class selection ........ OK
[slub] slab grow on demand ......... OK
[slub] free then reuse ............. OK
[slub] no leak after mixed free ..... OK
[slub] neighbour guard ............. OK
[slub] mixed random workload ....... OK
[slub] invalid & oversized request .. OK
[slub] cache re-init ................ OK
[slub test] all slub checks passed
```

![SLUB 自测结果](./images/lab2-slub-check.png)

*图 3：SLUB 自测（2026-10-29 本机实测，WSL2 Ubuntu + QEMU 10.2.1 + OpenSBI v1.8）。9 个用例全部通过，`slub_check()` 在 `pmm_init()` 之后执行。*

这里 `free pages` 由 31929 变成 31928，是内核链接结果变化导致的，并非分配器泄漏：加入 `slub_pmm.c` 后内核 footprint 从 25 KB 增到 29 KB，`end` 后移一页，`free physical memory` 的起点于是从 `0x80347000` 变成 `0x80348000`，`page descriptors` 也从 `0xffffffffc0207000` 挪到 `0xffffffffc0208000`；`32768 − 31928 = 840 = 512(OpenSBI) + 8(内核 29 KB 向上取整) + 320(页描述符)`，账仍然对得上。`[slub test]` 结尾的页数与开头完全相同，说明九个用例把借的页全部还清了。

`slub_check()` 在 `pmm_init()` 之后运行，且每个用例结束时都把第一层页数与对象计数还原，因此它不会改变课程注入测试（`[Test 1]`、`[Test 2]`）的判定结果；提交态下 `pmm_manager` 仍然指向 `best_fit_pmm_manager`（上面输出里的 `memory management: best_fit_pmm_manager` 即为证）。

---

### 分支任务：QEMU 双 GDB 调试地址转换过程

**实际复现与记录：** `[2510582 田城昊]`。本项是 Lab2 的独立分支任务；组内最终答辩分工由小组确认。

本任务同时从 QEMU 宿主侧和 uCore 客体侧观察一次地址翻译。三个 VS Code WSL 终端的职责如下：

| 终端   | 工具                      | 调试对象                                    |
| ------ | ------------------------- | ------------------------------------------- |
| 终端 1 | QEMU 4.1.1（带调试符号）  | 运行 uCore，并开放 1234 端口的 GDB stub     |
| 终端 2 | Ubuntu `/usr/bin/gdb`     | 附加 x86-64 QEMU 进程，在 QEMU 源码中断点   |
| 终端 3 | `riscv64-unknown-elf-gdb` | 通过远程协议调试 QEMU 中运行的 RISC-V uCore |

关键操作顺序为：

```text
QEMU: make debug（-s -S）
  -> Host GDB: attach QEMU PID
  -> handle SIGPIPE nostop noprint
  -> break get_physical_address
  -> RISC-V GDB: set remotetimeout unlimited
  -> target remote localhost:1234
  -> break kern_init
  -> continue
  -> Host GDB 命中 QEMU 地址翻译函数并检查参数、调用栈和返回值
```

本次在连接远程目标前执行 `set remotetimeout unlimited`，以便建立连接时就使用该设置。宿主 GDB 暂停 QEMU 时，QEMU 的 GDB stub 也会暂停；若 RISC-V GDB 此时等待回复，有限超时可能产生 `Ignoring packet error`。这属于双层调试的时序问题，不是 uCore 代码错误。

本次实测中，RISC-V GDB 的内存读取包经 QEMU gdbstub 进入如下调用链：

```text
handle_read_mem()
  -> target_memory_rw_debug()
  -> cpu_memory_rw_debug()
  -> cpu_get_phys_page_attrs_debug()
  -> riscv_cpu_get_phys_page_debug()
  -> get_physical_address()
```

Host GDB 观察到：

```text
addr        = 0xffffffffc0200000
access_type = 0
mmu_idx     = 1
return      = 0
phys_addr   = 0x80200000
```

返回值 `0` 表示翻译成功，且：

```text
0xffffffffc0200000 - 0x80200000 = 0xffffffff40000000
```

该差值与内核使用的 `va_pa_offset` 一致，说明高半虚拟地址 `0xffffffffc0200000` 被正确映射到内核加载物理地址 `0x80200000`。CPU 执行路径上的地址翻译可能命中 QEMU 软件 TLB，因此一次单步不保证进入该函数。本次截图中的调用栈明确属于 gdbstub 调试内存读取路径，该路径调用 `riscv_cpu_get_phys_page_debug()` 进行翻译，不能据此宣称已观察到 CPU 的 TLB miss 或回填过程。

`entry.S` 中启动页表最后一个根级页表项为有效叶子项，覆盖 1 GiB。Sv39 最多使用三级页表，但本次高半地址映射会在根级叶子项结束查询；报告不把本次结果描述成遍历了三级页表。

调试过程中还需区分 `ps -ef` 的 PID 和 PPID。本次 QEMU 行中的第一个数字才是 QEMU PID，第二个数字是父进程 `make` 的 PID；误附加父进程会出现自动识别执行文件为 `/usr/bin/make` 的提示。

关键截图：

![三终端联调并停在 kern_init](./images/分支任务/02_三终端联调并停在kern_init.png)

![QEMU 命中地址转换并显示参数](./images/分支任务/03_QEMU命中地址转换并显示参数.png)

![虚拟地址成功转换为物理地址](./images/分支任务/05_虚拟地址成功转换为物理地址.png)

---

TLB 填入与命中的逐步操作、源码注释和五张截图另见 [TLB 调试说明](./Lab2分支任务_TLB填入与命中逐步说明.md)。

## 五、测试与验证

### 5.1 模块一与模块二（提交状态：Best-Fit）的验证

在 WSL 中使用课程裸机工具链和 QEMU 4.1.1 执行：

```bash
cd "$LAB2_DIR"
make GCCPREFIX="$RISCV_PREFIX" clean
make GCCPREFIX="$RISCV_PREFIX" -j2
make GCCPREFIX="$RISCV_PREFIX" QEMU="$QEMU_411" qemu
make GCCPREFIX="$RISCV_PREFIX" QEMU="$QEMU_411" grade
```

关键运行结果：

```text
Physical Memory from DTB:
  Base: 0x0000000080000000
  Size: 0x0000000008000000 (128 MB)

physical memory: [0x0000000080000000, 0x0000000088000000)
free physical memory: [0x0000000080347000, 0x0000000088000000)
check_alloc_page() succeeded!

check page init: OK
Total Score: 20/20
```

测试含义：

1. 设备树成功报告 128 MiB 物理内存。
2. `page_init()` 正确设置全局状态，内核末尾页和描述符数组保持 Reserved。
3. `page2pa()` 与 `pa2page()` 能正确往返转换。
4. 空闲区成功注册给当前 `pmm_manager`，`check_alloc_page()` 能从该区域完成测试分配。

**测试截图：**

![DTB 内存探测与 page_init 初始化成功](./images/模块一/01_DTB内存探测与page_init初始化成功.png)

此图证明本次内核初始化和页面分配自测通过。上面的 `20/20` 是此前评分记录，本次文档合并没有重新评分，最终评分截图仍待补充；该成绩也不单独证明 Buddy 自测通过。

### 5.2 任务三（Buddy System）的单独验证

Buddy 不参与课程自动评分（`make grade` 只检查 `[Test 1]` 与 `[Test 2]`，后者仅判断 `pmm_manager` 是否为 `best_fit_pmm_manager`），因此它的正确性靠分配器自带的 5 项测试链来证明。验证步骤：

```bash
# 1. 把 kern/mm/pmm.c 中的 LAB2_VERIFY_BUDDY_PMM 改为 1
make GCCPREFIX="$RISCV_PREFIX" clean
make GCCPREFIX="$RISCV_PREFIX" -j2
make GCCPREFIX="$RISCV_PREFIX" QEMU="$QEMU_411" qemu    # 观察 buddy 自测输出并截图

# 2. 改回 0，确认提交状态仍然是 Best-Fit
make GCCPREFIX="$RISCV_PREFIX" clean
make GCCPREFIX="$RISCV_PREFIX" -j2
make GCCPREFIX="$RISCV_PREFIX" QEMU="$QEMU_411" grade   # 期望仍为 20/20
```

关键输出见「四、实验内容与实现 · 任务三 · 验证结果」一节。测试的设计思路是"每个用例前后对账"：基本分配回收、分配粒度、分裂与合并、越界请求、耗尽路径五个用例各覆盖一类路径，任何一个用例漏还页面或写错标记，都会在 `nr_free` 与链表结构的对比中立即暴露。

若本机暂时没有可用的交叉工具链，也可以用纯逻辑方式先行自检：把两个分配器的算法逐行等价复现后在多种"内核结束地址"下重跑同一批断言，确认算法不依赖某一次运行的具体地址，再上机验证。这种方式不能替代真实编译与运行，但能在没有环境时先排除逻辑错误。

---

### 5.3 样例检查：Buddy 与 SLUB 的测试用例设计与实现边界

本节按"实现边界 → 对应用例 → 关键断言 → 若缺失会漏掉什么"四段式，说明两个分配器的测试用例是怎样被设计出来的。三条判定标准贯穿全节：

1. **可复现**：用例只依赖框架保证的量（页数、`nr_free_pages()`、对齐关系），不依赖某一次运行得到的具体虚拟地址或物理地址。
2. **可归因**：一个用例只把一个变量推到边界上，其余条件保持默认，失败时能直接指到是哪个边界没处理。
3. **可对账**：每次分配/释放之后，`nr_free_pages()` 必须等于"所有空闲块页数之和"，把野指针、漏释放、重复释放这类错误变成可断言的数字差异，而不是等到某次莫名崩溃。

#### 5.3.1 Buddy System 的实现边界清单

以下边界对应提交仓库中的 `kern/mm/buddy_system_pmm.c`（`BUDDY_MAX_ORDER` 取 19，各阶空闲块挂 `free_list[order]`，块头页 `property` 记录块大小、`PageProperty` 标记"空闲块头页"）。

| 编号 | 边界情况 | 实现中的处理方式 | 覆盖用例 |
| --- | --- | --- | --- |
| B1 | 请求 1 页（最小粒度） | `buddy_order_of(1)` 得 0 阶，直接取 0 阶块，不分裂 | `buddy_basic_check` |
| B2 | 请求非 2 的幂（当前用例为 3 页） | 向上取整到 4、8 页，实际块大小记在块头 `property`，**不是**请求值 | `buddy_order_check` |
| B3 | 请求恰等于某个空闲块的大小 | 不分裂，整块移出链表 | `buddy_split_merge_check`（3.1） |
| B4 | 请求小于所有非空链表的最小阶 | 从高阶块逐级二分，多出的高半页挂回低一阶链表 | `buddy_split_merge_check`（3.2） |
| B5 | 释放时找伙伴 | 伙伴页号 = 本块页号 `^` 块大小；伙伴必须同阶且 `PageProperty` 置位才合并 | `buddy_split_merge_check` |
| B6 | 连续多级合并 | 释放函数逐级查找同阶伙伴并尝试合并 | 现有测试检查回收与整块可再分配，但未隔离唯一伙伴对，不能证明每一级合并都执行过 |
| B7 | 伙伴超出描述符数组 | `buddy_idx >= npage - nbase` 时停止，不解引用数组外地址 | 无专门触发该分支的独立用例 |
| B8 | 请求超过空闲总页数或超过单块上限 | `n > nr_free` 或向上取整后仍大于 `2^19` 页 → 返回 `NULL`，且 `nr_free` 与链表都不变 | `buddy_limit_check` |
| B9 | 反复分配直到没有合适块 | 返回 `NULL`；已分配块全部释放后计数完全还原 | `buddy_exhaust_check` |
| B10 | 非 2 的幂请求的释放 | 申请 3 页实占 4 页，按原请求 3 页释放时读取块头 property 回收 4 页；任意错误长度不受支持 | `buddy_order_check` |
| B11 | 非法参数（`n = 0`） | `assert(n > 0)` 直接失败；框架的 `alloc_page()/free_page()` 与注入的课程测试都不会传 0，属"约定不出现"的边界，用断言显式封死而不是静默处理 | 无独立用例（由断言兜底） |
| B12 | 每次操作后的结构不变量 | 链表里只有空闲块头页、块大小必须等于所在阶、块必须按自身大小对齐、全部块页数之和等于 `nr_free` | 由 `buddy_sum_free_pages()` 在每个用例首尾各对账一次 |

五个测试覆盖基本分配、取整、分裂回收、超量请求及固定大块分配失败。伙伴越界分支和连续多级合并的逐级执行尚未单独构造测试。

#### 5.3.2 Buddy 的测试用例设计

| 用例 | 构造方式 | 关键断言 | 若缺失会漏掉什么 |
| --- | --- | --- | --- |
| `buddy_basic_check` | 连续分配 3 个单页再全部释放 | 三个指针非空且互不相同、`page_ref == 0`、`page2pa < npage * PGSIZE`、`nr_free` 减 3 再还原 | 0 阶链表的插入/摘除错误、分配结果重叠 |
| `buddy_order_check` | `alloc_pages(3)`，再 `free_pages(p, 3)` | 块起始地址按 4 页对齐、`property == 4`、`!PageProperty(p)`、`nr_free` 减 4 再还原 | 把"请求 3 页"直接当块大小记账，导致释放时只回收 3 页、每轮泄漏 1 页 |
| `buddy_split_merge_check` | 先整块分配/释放最小阶；再把当前最大块切成两半分别释放，最后重新申请整块 | 两步之后 `nr_free` 都回到初值；最后一次整块申请必须成功 | 分裂时把"多出的另一半"挂错阶；释放时不合并（碎片永久化） |
| `buddy_limit_check` | 申请 `npage - nbase` 页、申请 `nr_free + 1` 页 | 两次都返回 `NULL`，且 `nr_free` 与链表总和不变 | 越界请求被当成合法请求，写到受管区间之外 |
| `buddy_exhaust_check` | 每块 4096 页反复分配至失败（上限 32 块），再逆序全部释放 | 失败后 `nr_free == free_before - cnt * 4096`；释放后完全还原 | 耗尽路径的计数错误、以及"部分释放顺序"下的链表损坏（逆序释放恰好触发不同顺序的合并） |

设计上有两点刻意为之：

- **释放顺序不固定**：`buddy_exhaust_check` 逆序释放，`buddy_basic_check` 顺序释放，两种顺序触发不同的合并链，避免实现只对某一种顺序正确。
- **不假设固定页数**：所有断言都写成"相对初值的变化量"，因此内核镜像大小变化导致 `31929` 变成别的数字时，用例依然有效。

#### 5.3.3 SLUB 模块的两层结构与实现边界

指导书对 Challenge（SLUB）的要求是"第一层按页分配、第二层按任意大小分配"，允许简化实现但必须"能够正常编译运行"，并且要提交两份 prompt。实际实现落在 `kern/mm/slub_pmm.c` 与 `kern/mm/slub_pmm.h`，由 `kern/init/init.c` 在 `pmm_init()` 之后调用 `slub_init(); slub_check();`。下面先给出实现的结构约定，再给出边界清单与用例设计——它们同时是两份 prompt 的规格来源，也是验收清单。

结构约定：

- **页级层**：对象层不自己管理物理页，`slub_new_slab()` 用 `alloc_pages(1)` 要 1 页、`slub_destroy_slab()` 用 `free_pages(page, 1)` 归还，因此提交态（Best-Fit）与 Buddy 自测态都能垫在下面。
- **对象级层**：把 slab 切成等长对象，按 size class 分档（8、16、32、64、128、256、512、1024、2048 字节），每个 cache 维护 `slabs` 链、`slab_count`、`inuse`；每页可切 504/252/126/63/31/15/7/3/1 个对象。
- **元数据位置**：`struct slub_slab`（RV64 下为 40 字节）放在 slab 首页的前 64 字节，并用编译期断言 `sizeof(struct slub_slab) <= 64` 固定；对象区是 `[page + 64, page + 4096)`。按 4 KiB 取下界可以定位候选页；当前释放实现实际通过扫描 slab 链查找归属。
- **空闲链**：空闲对象的前 8 字节存"下一个空闲对象地址"，LIFO；因此最小档位必须不小于 `sizeof(void *)`。
- **回收**：`inuse == 0` 时立即销毁 slab 并把页还给页级层，于是"全部释放后页数回到初值"成为可直接断言的性质。
- **直通路径**：请求超过 2048 字节时不走对象层，向页级层要 `ceil((size + 16) / PGSIZE)` 页，页首 16 字节写 magic 与页数，返回 `header + 16`。
- **归属判定**：释放只有指针、没有长度参数，先按地址区间判断是否落在某个 slab 内，命中则按该档位回收，否则按直通头的页数归还。

| 编号 | 边界情况 | 实现中的处理方式 | 检验要点 |
| --- | --- | --- | --- |
| S1 | 申请 `size = 0` | `slub_alloc` 最先判 `size == 0` 返回 `NULL`，不进入档位查找 | 空尺寸不得绕过档位查找 |
| S2 | 申请大小恰为档位值（8/16/…/2048） | `slub_class_of` 返回第一个 `size <= 档位` 的下标，恰好命中本档 | 档位查找的 off-by-one |
| S3 | 申请 2048 与 2049 字节 | 2048 走对象层（每页 1 个对象）；2049 超过 `SLUB_MAX_OBJECT`，走直通路径取 1 页 | 档位上限与直通路径的切换点 |
| S4 | 直通路径的页数 | `ceil((size + 16) / PGSIZE)`：2049 → 1 页，5000 → 2 页 | 头部预留导致少算一页会越界写 |
| S5 | 单 slab 的对象用尽 | `freelist == NULL` 时向页级层新建 slab，绝不超过 `page + 4096` 继续切分 | 越界写会把相邻 slab 的对象链踩坏 |
| S6 | slab 内最后一个对象被释放 | `inuse == 0` → 从 cache 链摘除 → `free_pages()` 归还整页 | 不还页则泄漏检查无从对账 |
| S7 | 释放后立即重新申请 | freelist 头插、分配时摘头，必然先返回刚释放的对象 | 空闲链插入/摘除错误会换出别的地址 |
| S8 | 对象边界 | 写满合法对象范围后，检查相邻对象及元数据未改变；不提供通用越界写检测 | `slub_guard_check` |
| S9 | `slub_free(NULL)` | 直接返回；本次在 `slub_oom_check` 中增加显式调用 | `slub_oom_check` |
| S10 | 非法指针或重复释放 | 不受支持；内部断言只覆盖部分情况，任意指针可能先触发访存异常，同一 slab 内重复释放可能破坏空闲链 | 无负向测试，调用方必须保证指针合法且只释放一次 |
| S11 | 页级层分配失败（1 TiB 请求） | `alloc_pages` 返回 `NULL` → `slub_alloc` 返回 `NULL`，页数与计数不变 | 失败路径不得留下半初始化状态 |
| S12 | cache 重新初始化 | `slub_init()` 重置九个 cache 的链与计数，此后仍可正常分配 | 全局状态未复位 |
| S13 | 混合尺寸随机申请/释放 | 各 cache 计数独立；直通对象不计入 cache 的已用对象数 | 统计口径串扰会制造假阳性 |
| S14 | 结构检查 | `slub_structure_ok()` 检查 canary、空闲对象区间、相对档位对齐、空闲数与 inuse 之和，并限制遍历长度 | 由部分用例及测试末尾显式调用，不是每次操作都会检查 |

#### 5.3.4 SLUB 的测试用例设计

测试写在 `kern/mm/slub_pmm.c` 内，由 `slub_check()` 依次调用（函数名与实现中的真实名字一致）：

| 用例（实现函数） | 构造方式 | 关键断言 | 检验的边界 |
| --- | --- | --- | --- |
| 基本分配回收（`slub_basic_check`） | 最小档 8 字节连续申请 3 个对象 | 三个地址互不相同、落在同一页内、地址差为 8 的整数倍、slab 数为 1、已用对象数为 3；全部释放后 slab 归零、页数还原 | S1、S2、S6、S7 |
| 档位选择（`slub_size_class_check`） | 九个档位各申请一次，再申请 2049 | 地址按 8 字节对齐，档位 ≤ 64 时还按自身大小对齐；2049 走直通路径；释放后页数还原 | S2、S3、S4 |
| slab 扩容（`slub_slab_grow_check`） | 64 字节档连续申请 64 个（单 slab 只有 63 个） | slab 数恰好变为 2、已用对象数为 64、页数减少 2；释放后页数还原 | S5 |
| 释放后重用（`slub_free_reuse_check`） | 申请 32 字节 → 释放 → 再申请同尺寸 | 第二次必须返回与第一次完全相同的地址 | S7 |
| 泄漏检查（`slub_leak_check`） | 六个尺寸 `{8, 64, 256, 1024, 2048, 3000}` 各 3 次（含直通），全部逆序释放 | slab 数为 0、已用对象数为 0、页数与开始时完全相等 | S4、S6、S13 |
| 越界防护（`slub_guard_check`） | 64 字节档申请相邻两个对象，写满低地址者的 64 字节 | 高地址对象内容不变、元数据 canary 完好 | S8 |
| 混合负载（`slub_mixed_check`） | 伪随机 200 步，尺寸 `{8, 100, 600, 2049, 5000, 12}` 随机申请/释放 | 每一步都通过结构自检；已用对象数等于现场 slab 对象数；结束时页数完全还原 | S13、S14 |
| 非法与超大请求（`slub_oom_check`） | `slub_alloc(0)`、`slub_alloc(1 TiB)` | 两者都返回 `NULL`，页数与计数不变 | S1、S11 |
| cache 重建（`slub_cache_reuse_check`） | 再次执行 `slub_init()`，随后申请 2048 字节 | 重建后 slab 数与对象总数归零；2048 档恰好 1 个 slab、1 个对象，释放后归零 | S12 |

#### 5.3.5 为什么这批用例能够"有效检验"

1. **每个用例只推一个边界**。例如 `buddy_order_check` 只改"请求页数不是 2 的幂"这一个条件，其余保持默认，因此一旦失败，问题必然出在"按 2 的幂取整 + 块大小记账"这条链路上，而不是"某个随机地址恰好有问题"。
2. **用计数对账代替肉眼检查**。分配器的错误大多不立刻崩溃，而是表现为"少了一页""多了一页""某个块永远回不来"。`buddy_sum_free_pages()`、`slub` 的 `freelist` 总长与 `nr_free_pages()` 三者对齐，把这类静默错误变成确定的数字不等。
3. **让越界变成崩溃而不是污染**。对齐断言（块起点必须按自身大小对齐）、哨兵字节、`assert(!PageReserved(p))`，都属于"错了就立刻停在这里"，而不是让它继续跑、在别处莫名其妙地挂掉。
4. **覆盖"失败路径"而不只是成功路径**。`buddy_limit_check`、`buddy_exhaust_check`、`slub_oom_check` 检验的都是"拿不到内存时不能把已有状态弄坏"，这是分配器最容易漏测、也最容易在真实系统里出问题的地方。
5. **不绑定具体数值**。所有期望值都写成"相对初值的变化量"，内核大小或 QEMU 内存尺寸改变后用例仍然成立，可重复执行。

#### 5.3.6 样例检查问答准备

| 可能被问到 | 回答要点 |
| --- | --- |
| 为什么 Buddy 只能按 2 的幂给出块？ | 因为伙伴定位依赖位运算 `idx ^ size`，只有块大小为 2 的幂、且起点按自身大小对齐时，异或的结果才正好是伙伴块的页号。 |
| 请求 3 页却拿到 4 页，多出来的 1 页去哪了？ | 变成内部碎片留在该块内部，直到这块被释放才回到空闲链表。这是用"分配快、合并快"换来的空间代价，`property` 记录真实大小就是为回收时不吃亏。 |
| `property` 和 `PageProperty` 分别做什么？ | `property` 记录块包含多少页（数值），`PageProperty` 标记该页是不是空闲块的头页（布尔位）。释放时先看布尔位判断"是不是空闲块头"，再读数值得到块大小。 |
| 释放时为什么不直接用传入的 `n`？ | 调用方可能只知道自己请求了 3 页，而实际块是 4 页；以块头 `property` 为准才不会每次泄漏 1 页。 |
| 空闲页数不是 2 的幂，`init_memmap` 怎么建链表？ | 不从整块开始切：从区间起点出发，取"本阶大小 ≤ 剩余页数、且起点按本阶大小对齐"的最大阶切一块挂上去，指针后移一阶继续，直到区间用完。本机最终构建（起点 `0x80348000`、31928 页）拆成 8、16、32、128、1024、2048、4096、8192、16384 页共 9 块。 |
| `BUDDY_MAX_ORDER` 为什么取 19？ | 受管区间从 `DRAM_BASE` 起算，`nbase = DRAM_BASE / PGSIZE = 2^19` 页，2^19 页以内的块在描述符下标与物理页号两种视角下同时对齐。 |
| SLUB 为什么要分两层？ | 页级层只会分配 2 的幂页，难以满足任意大小的对象；对象层把 slab 切成等长对象，用空间换分配速度，同时避免每个小对象都占一页。 |
| 怎么证明你的分配器没有泄漏？ | 每个用例首尾都对账：全部空闲块页数之和 == `nr_free_pages()`，用例结束后总页数回到初值。 |
| SLUB 里"这个对象多大"记在哪？ | 记在 slab 所属的 `kmem_cache` 里（九档之一），释放时靠"指针落在哪个 slab 的地址区间"反查档位，所以 `slub_free(ptr)` 不需要长度参数。 |
| 为什么元数据放页首 64 字节，而不是用一个静态数组？ | 静态数组要么限制 slab 总数，要么需要用地址去查表；放页首后"页 → slab"是常数时间，且销毁时元数据随页一起走，不残留。`struct slub_slab` 40 字节，用编译期断言保证不超过 64 字节。 |
| 为什么 slab 变空就把页还给页级层？ | 让"泄漏"可测量：每个用例结束时页数必须回到初值，如果空 slab 一直挂着不还，页数就对不上，内存看起来也一直占着。 |
| 请求 3000 字节（超过 2048 档）怎么处理？ | 走直通路径：按 `ceil((size+16)/4096)` 向页级层要连续页，页首 16 字节写 magic 与页数，返回 `header+16`；释放时读这个头把整段还回去。 |
| 怎么知道一个空指针或野指针不会被当成合法指针释放？ | `slub_free(NULL)` 直接返回；其余指针先按地址区间匹配 slab，匹配不上再按直通头校验 magic，`SLUB_BIG_MAGIC` 对不上就 `panic`，宁可停机也不写坏别处。 |

---

## 六、实验总结与收获

### 对操作系统的理解

1. 物理内存管理不是单纯实现一个分配算法。系统必须先从平台信息中发现 RAM，再扣除固件、内核和元数据占用区，最后才能建立分配器状态。
2. `struct Page` 是物理页的软件描述，而不是物理页本身。`pages[i]` 与某个物理页一一对应，但描述符数组也需要占用实际内存。
3. 虚拟地址、物理地址、物理页号和描述符指针是四种不同表示。`PADDR`、`PPN`、`pa2page` 和 `page2pa` 分别承担不同转换职责，混用会产生越界或错误映射。
4. 页面起点需要向上对齐，内存末端需要向下对齐，这样注册给分配器的每一页才完全落在安全范围内。
5. `pmm_manager` 将内存布局初始化与具体分配策略解耦；功能模块一只负责提供安全的初始空闲页。
6. 同一批空闲页可以用完全不同的结构组织：First-Fit 与 Best-Fit 用一条按地址排序的链表、靠 `property` 记录块长度；Buddy 用每一阶一条链表，靠"块大小是 2 的幂"这一约束让块首自动对齐，从而能用异或直接算出伙伴。数据结构的选择决定了能做哪种合并，也决定了碎片的形态——前两者产生外部碎片，Buddy 产生内部碎片。
7. 分配接口的语义并不总是"给我 n 页"：Buddy 只能按 2 的幂给出，于是"真实块大小"必须由分配方记录、由释放方读取。这类记账错误在测试里往往表现为难以定位的越界或链表结构错乱，而不是直接的崩溃。
8. "外部评测"与"自测"是两种不同性质的东西：课程注入测试只验证规定行为（`page_init` 的八项断言、管理器选择是否正确），像 Buddy 这种没有自动评分的实现必须自己补测试，而且测试里要有对账步骤，才能验证"没有泄漏"而不只是"能分配"。

OS 原理中重要、但本模块尚未完整覆盖的内容包括：NUMA 拓扑、内存热插拔、坏页隔离、DMA 区域约束、多段 RAM 和页回收。本实验使用单核、固定 QEMU 内存布局和简化 DTB 解析器，重点是建立物理内存初始化的基础概念和接口。

### AI 协作开发的经验

1. 提示词必须写清可依赖接口、前置条件、后置条件和异常分支，仅描述“实现 page_init”不足以约束地址单位和保留区边界。
2. AI 生成或修改内核代码后仍需人工阅读反汇编、地址转换和数据结构不变量；能编译不代表能在分页前的启动环境中执行。
3. 环境问题和代码问题要分开定位。本次无输出不是 QEMU 4.1.1 故障，而是错误工具链产生 GOT/PIC 寻址；对比反汇编后才能确定根因。
4. 课程测试可验证规定行为，但不能替代边界分析。实验实现固定 `DRAM_BASE`、只解析一个 RAM 区间，报告中必须明确这些限制。
5. 让 AI 从零写一个算法时，把"不变量"写进 `[SPECIFICATION]` 比写实现步骤更有效：例如"各阶空闲块页数之和必须等于 nr_free"，它会把记账补齐；而我只需要在测试里核对这一条。
6. 只让 AI 生成实现是不够的，还要让它生成测试，但提示词里必须写明"每个用例前后必须恢复状态"，否则生成的测试很容易把分配器改到半损坏状态，后面的用例全都在用坏掉的堆。
7. 提示词里明确"不得改动某几个文件"很有价值：任务三只需要新增文件，但 AI 很容易顺手去改 `init_pmm_manager()`，那样会把已经通过的 `[Test 2]` 弄掉。

---

## 合并检查说明（2026-10-30）

本次合入成员三提供的 Buddy、SLUB 和测试设计，保留模块一 page_init、Best-Fit 实现和 QEMU 4.1.1 配置。组员的 lab2-grade-local.png 来自自编 grade_new_qemu.sh，仅作为其环境下的自查记录，不能代替课程 make grade。

当前合并版本的课程 make grade 由组长在 VS Code 执行；正式评分截图尚未补入。历史 20/20 不代表已对本次新增 SLUB 的最终代码重新评分。

SLUB 不支持非法指针、重复释放、并发和有存活对象时重新初始化。对象仅保证至少 8 字节对齐，不能宣称所有档位都按自身大小对齐。直通分配新增加法与页对齐溢出检查；对应自测加入 SIZE_MAX 附近请求、free(NULL) 和 2048/2049 档位切换。

更详细的测试覆盖与限制见 [Buddy 与 SLUB 合并检查](./Buddy与SLUB合并检查.md)。

2026-10-30 合并预检：Best-Fit + SLUB 和 Buddy + SLUB 均编译并启动自测通过，详细输出见合并检查及其日志；课程 make grade 尚未重跑。
