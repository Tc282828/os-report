#include <pmm.h>
#include <list.h>
#include <stdio.h>
#include <buddy_system_pmm.h>

//lab2 function2-3 2511203
/* Buddy System（伙伴系统）连续物理内存分配器
 *
 * 结构：所有空闲块的长度都是 2 的幂（2^order 个页），同一阶的空闲块挂在
 * 同一条链表上。块头页的 property 记录块大小，PageProperty 位标记“这是一个
 * 空闲块的头页”。
 *
 * 分配：把请求页数向上取整到 2^k，从第 k 阶开始向上找第一个非空链表，取出
 * 其中的块，再逐级二分（分裂），多出来的另一半作为一个低一阶的空闲块挂回链表。
 * 分配出去的块头页仍保留 property = 2^k，但清除 PageProperty，这样释放时能
 * 还原真实块大小——Buddy 只能按 2 的幂回收，这是它与 First-Fit/Best-Fit 的
 * 关键差别（会带来内部碎片）。
 *
 * 释放：块头页页号异或其块大小即为伙伴页号；若伙伴块同阶且空闲，则合并且阶数
 * 加一，直到无法继续合并为止。
 *
 * 本实验的受管区间从 DRAM_BASE 开始，nbase = DRAM_BASE / PGSIZE = 2^19 页，
 * 因此 2^19 页以内的块在“描述符下标”和“物理页号”两种视角下都是对齐的，
 * BUDDY_MAX_ORDER 取 19 即可覆盖 2 GiB；QEMU virt 默认为 128 MiB，实际空闲区
 * 只有约 3.2 万页，最大块的阶为 14 左右。 */

#define BUDDY_MAX_ORDER 19

#define BUDDY_TEST_CHUNK_PAGES 4096     /* 批量分配测试的单块页数（16 MiB） */
#define BUDDY_TEST_MAX_CHUNKS  32

typedef struct {
    list_entry_t free_list[BUDDY_MAX_ORDER + 1];  /* 各阶空闲块链表 */
    unsigned int nr_free;                         /* 空闲页总数 */
} buddy_free_area_t;

static buddy_free_area_t buddy_area;

/* ---------- 基础换算 ---------- */

static size_t buddy_pages_of_order(unsigned int order) {
    return ((size_t)1) << order;
}

/* 返回最小的 order，使 2^order >= pages */
static unsigned int buddy_order_of(size_t pages) {
    unsigned int order = 0;
    while (order < BUDDY_MAX_ORDER && buddy_pages_of_order(order) < pages) {
        order++;
    }
    return order;
}

static bool buddy_is_power_of_two(size_t x) {
    return x != 0 && (x & (x - 1)) == 0;
}

/* 把请求页数换算成阶，并顺带判定"是否超过单块上限"。
 *
 * Buddy 一次只能切出 2^order 页，而 order 被 BUDDY_MAX_ORDER 截断，所以超过
 * 2^BUDDY_MAX_ORDER 页的请求虽然可能小于 nr_free，仍然必须拒绝。把这个判定
 * 单独成一个函数，测试才能在没有 2 GiB 内存的机器上直接验证这条分支。 */
static unsigned int buddy_order_for_request(size_t n, bool *too_large) {
    unsigned int order = buddy_order_of(n);
    *too_large = (buddy_pages_of_order(order) < n);
    return order;
}

/* ---------- 空闲块链表操作 ---------- */

/* 把 base 作为第 order 阶的空闲块头页挂到对应链表（不计入 nr_free） */
static void buddy_push_free_block(struct Page *base, unsigned int order) {
    base->property = (unsigned int)buddy_pages_of_order(order);
    SetPageProperty(base);
    list_add(&buddy_area.free_list[order], &base->page_link);
}

/* ---------- pmm_manager 需要的六个接口 ---------- */

static void buddy_init(void) {
    unsigned int order;
    for (order = 0; order <= BUDDY_MAX_ORDER; order++) {
        list_init(&buddy_area.free_list[order]);
    }
    buddy_area.nr_free = 0;
}

static void buddy_init_memmap(struct Page *base, size_t n) {
    assert(n > 0);

    struct Page *p = base;
    for (; p != base + n; p++) {
        assert(PageReserved(p));
        p->flags = 0;
        p->property = 0;
        set_page_ref(p, 0);
    }

    /* 把 [base, base + n) 拆成若干个“起点对齐、大小为本阶”的 2^order 块 */
    while (n > 0) {
        size_t idx = (size_t)(base - pages);
        unsigned int order = BUDDY_MAX_ORDER;
        while (order > 0) {
            size_t size = buddy_pages_of_order(order);
            if (size <= n && (idx % size) == 0) {
                break;
            }
            order--;
        }
        size_t size = buddy_pages_of_order(order);  /* order == 0 时一定可用 */
        buddy_push_free_block(base, order);
        buddy_area.nr_free += (unsigned int)size;
        base += size;
        n -= size;
    }
}

static struct Page *buddy_alloc_pages(size_t n) {
    assert(n > 0);
    if (n > buddy_area.nr_free) {
        return NULL;
    }
    bool too_large = 0;
    unsigned int order = buddy_order_for_request(n, &too_large);
    if (too_large) {
        return NULL;  /* 超过单块上限（2^BUDDY_MAX_ORDER 页） */
    }

    /* 找第一个有块的阶 */
    unsigned int cur = order;
    while (cur <= BUDDY_MAX_ORDER && list_empty(&buddy_area.free_list[cur])) {
        cur++;
    }
    if (cur > BUDDY_MAX_ORDER) {
        return NULL;
    }

    struct Page *block = le2page(list_next(&buddy_area.free_list[cur]), page_link);
    list_del(&block->page_link);
    ClearPageProperty(block);

    /* 逐级二分，把多出来的另一半挂到低一阶的链表 */
    while (cur > order) {
        cur--;
        buddy_push_free_block(block + buddy_pages_of_order(cur), cur);
    }

    /* 记录真实块大小（2^order 页），供 free_pages 还原 */
    block->property = (unsigned int)buddy_pages_of_order(order);
    buddy_area.nr_free -= (unsigned int)buddy_pages_of_order(order);
    return block;
}

static void buddy_free_pages(struct Page *base, size_t n) {
    assert(n > 0);
    assert(!PageReserved(base));

    /* 真实块大小：优先用分配时记录在块头页上的大小，否则把 n 向上取整到 2 的幂 */
    size_t size = (size_t)base->property;
    if (size < n) {
        size = n;
    }
    if (!buddy_is_power_of_two(size)) {
        size = buddy_pages_of_order(buddy_order_of(size));
    }

    struct Page *p;
    for (p = base; p != base + size; p++) {
        assert(!PageReserved(p));
        p->flags = 0;
        p->property = 0;
        set_page_ref(p, 0);
    }
    buddy_area.nr_free += (unsigned int)size;

    unsigned int order = buddy_order_of(size);
    /* 伙伴页号 = 本块页号 ^ 块大小，反复向上合并 */
    while (order < BUDDY_MAX_ORDER) {
        size_t idx = (size_t)(base - pages);
        size_t buddy_idx = idx ^ buddy_pages_of_order(order);
        if (buddy_idx >= (size_t)(npage - nbase)) {
            break;
        }
        struct Page *buddy = pages + buddy_idx;
        if (!PageProperty(buddy) ||
            (size_t)buddy->property != buddy_pages_of_order(order)) {
            break;
        }
        list_del(&buddy->page_link);
        ClearPageProperty(buddy);
        buddy->property = 0;
        order++;
        if (buddy < base) {
            base = buddy;
        }
    }
    buddy_push_free_block(base, order);
}

static size_t buddy_nr_free_pages(void) {
    return (size_t)buddy_area.nr_free;
}

/* ---------- 自带的测试函数（任务三第二份 prompt 的产物） ---------- */

/* 统计某一阶的空闲块个数（测试用） */
static unsigned int buddy_count_blocks(unsigned int order) {
    unsigned int count = 0;
    list_entry_t *head = &buddy_area.free_list[order];
    list_entry_t *le = head;
    while ((le = list_next(le)) != head) {
        count++;
    }
    return count;
}

/* 判断某个页是否正作为第 order 阶空闲块的头页挂在链表上（测试用） */
static bool buddy_free_list_contains(struct Page *target, unsigned int order) {
    list_entry_t *head = &buddy_area.free_list[order];
    list_entry_t *le = head;
    while ((le = list_next(le)) != head) {
        if (le2page(le, page_link) == target) {
            return 1;
        }
    }
    return 0;
}

/* 遍历所有阶的空闲链表：检查块头状态、块大小与所在阶一致、块按自身大小对齐，
 * 并返回所有空闲块的页数之和，用于与 nr_free 对账。 */
static unsigned int buddy_sum_free_pages(void) {
    unsigned int total = 0;
    unsigned int order;
    for (order = 0; order <= BUDDY_MAX_ORDER; order++) {
        list_entry_t *head = &buddy_area.free_list[order];
        list_entry_t *le = head;
        while ((le = list_next(le)) != head) {
            struct Page *p = le2page(le, page_link);
            size_t size = buddy_pages_of_order(order);
            assert(PageProperty(p));                        /* 链表里只能是空闲块头页 */
            assert((size_t)p->property == size);            /* 块大小必须等于所在阶 */
            assert(((size_t)(p - pages) % size) == 0);      /* 块必须按自身大小对齐 */
            total += p->property;
        }
    }
    return total;
}

/* 测试 1：单页分配与释放，引用计数与空闲页计数必须严格还原 */
static void buddy_basic_check(void) {
    unsigned int free_before = buddy_area.nr_free;
    struct Page *p0 = alloc_page();
    struct Page *p1 = alloc_page();
    struct Page *p2 = alloc_page();

    assert(p0 != NULL && p1 != NULL && p2 != NULL);
    assert(p0 != p1 && p0 != p2 && p1 != p2);
    assert(page_ref(p0) == 0 && page_ref(p1) == 0 && page_ref(p2) == 0);
    assert(page2pa(p0) < npage * PGSIZE);
    assert(buddy_area.nr_free == free_before - 3);
    assert(buddy_sum_free_pages() == buddy_area.nr_free);

    free_page(p0);
    free_page(p1);
    free_page(p2);
    assert(buddy_area.nr_free == free_before);
    assert(buddy_sum_free_pages() == buddy_area.nr_free);
    cprintf("  [buddy] single page alloc/free .......... OK\n");
}

/* 测试 2：分配粒度必须是 2 的幂，块头页要记录真实块大小并保持对齐 */
static void buddy_order_check(void) {
    unsigned int free_before = buddy_area.nr_free;

    struct Page *p = alloc_pages(3);            /* 3 页只能给 4 页的块 */
    assert(p != NULL);
    assert((size_t)(p - pages) % 4 == 0);       /* 块按自身大小对齐 */
    assert((size_t)p->property == 4);           /* 块头记录真实大小 */
    assert(!PageProperty(p));                   /* 已分配，不再算空闲块头 */
    assert(buddy_area.nr_free == free_before - 4);

    free_pages(p, 3);                           /* 按原请求页数释放 */
    assert(buddy_area.nr_free == free_before);
    assert(buddy_sum_free_pages() == buddy_area.nr_free);
    cprintf("  [buddy] power-of-two granularity ........ OK\n");
}

/* 测试 3：分裂与合并。整块分配/释放后页数应还原；再让一次单页申请把某个高阶块
 * 一路分裂到底，逐级确认每一级的伙伴块都空闲在对应阶链表上，释放后各阶分布复原。 */
static void buddy_split_merge_check(void) {
    unsigned int free_before = buddy_area.nr_free;

    unsigned int order = 0;
    while (order <= BUDDY_MAX_ORDER && list_empty(&buddy_area.free_list[order])) {
        order++;
    }
    assert(order <= BUDDY_MAX_ORDER);

    /* 3.1 从最小的空闲块整块分配再释放 */
    size_t size = buddy_pages_of_order(order);
    struct Page *p = alloc_pages(size);
    assert(p != NULL);
    assert((size_t)p->property == size);
    assert((size_t)(p - pages) % size == 0);
    free_pages(p, size);
    assert(buddy_area.nr_free == free_before);

    /* 3.2 逐级合并：先把现成的 0 阶块借走（1 页构型里确实存在这样一块），否则
     * alloc_page() 会直接取走它、根本不发生分裂。借完之后再分配 1 页，必然从某个
     * 高阶块一路分裂下来——分裂时挂回链表的另一半，就是它在该级的伙伴。
     * 逐级核对"每一级的伙伴都在对应阶链表上、阶数正确"，再释放，要求各阶块数
     * 分布完全复原：只有合并一路级联回原来的高阶块，分布才会复原。所以这一条
     * 同时验证"伙伴关系"与"逐级合并"，不依赖"两次分配刚好拿到同一父块"这种运气。 */
    unsigned int cnt_orig[BUDDY_MAX_ORDER + 1];
    unsigned int cnt_before[BUDDY_MAX_ORDER + 1];
    unsigned int cnt_now[BUDDY_MAX_ORDER + 1];
    for (order = 0; order <= BUDDY_MAX_ORDER; order++) {
        cnt_orig[order] = buddy_count_blocks(order);
    }

    struct Page *pinned[8];
    unsigned int npinned = 0;
    while (npinned < 8 && buddy_count_blocks(0) > 0) {
        pinned[npinned] = alloc_page();
        assert(pinned[npinned] != NULL);
        npinned++;
    }
    assert(buddy_count_blocks(0) == 0);          /* 0 阶已经清空，下一次必然分裂 */
    assert(buddy_area.nr_free == free_before - npinned);
    for (order = 0; order <= BUDDY_MAX_ORDER; order++) {
        cnt_before[order] = buddy_count_blocks(order);
    }

    struct Page *one = alloc_page();
    assert(one != NULL);
    assert(buddy_area.nr_free == free_before - npinned - 1);

    /* 这次分裂的起点阶：唯一一个块数减少 1 的阶 */
    unsigned int split = BUDDY_MAX_ORDER + 1;
    for (order = 0; order <= BUDDY_MAX_ORDER; order++) {
        cnt_now[order] = buddy_count_blocks(order);
        if (cnt_now[order] + 1 == cnt_before[order]) {
            assert(split > BUDDY_MAX_ORDER);   /* 至多一个阶减少 1 */
            split = order;
        }
    }
    assert(split >= 1 && split <= BUDDY_MAX_ORDER);   /* 0 阶已清空，必然分裂 */

    size_t p_idx = (size_t)(one - pages);
    unsigned int k;
    for (k = 0; k < split; k++) {
        size_t size_k = buddy_pages_of_order(k);
        size_t parent = p_idx & ~((size_k << 1) - 1);   /* 这一级的父块起点 */
        size_t sib_idx = parent + ((p_idx & size_k) ? 0 : size_k);  /* 父块的另一半 */
        struct Page *sib = pages + sib_idx;
        assert(sib_idx < (size_t)(npage - nbase));
        assert((size_t)sib->property == size_k);
        assert(buddy_free_list_contains(sib, k));       /* 伙伴块确实空闲着 */
    }

    free_page(one);
    assert(buddy_area.nr_free == free_before - npinned);
    for (order = 0; order <= BUDDY_MAX_ORDER; order++) {
        assert(buddy_count_blocks(order) == cnt_before[order]);
    }

    /* 把借走的 0 阶块还回去，各阶分布应回到进入 3.2 之前的样子 */
    for (k = 0; k < npinned; k++) {
        free_page(pinned[k]);
    }
    assert(buddy_area.nr_free == free_before);
    for (order = 0; order <= BUDDY_MAX_ORDER; order++) {
        assert(buddy_count_blocks(order) == cnt_orig[order]);
    }
    assert(buddy_sum_free_pages() == buddy_area.nr_free);
    cprintf("  [buddy] split & merge .................. OK\n");
}

/* 测试 4：越界请求必须失败，且不改变空闲页数。
 *
 * 这里有两个不同的"拒绝"分支：
 *   a) 请求页数 > nr_free —— 在 128 MiB 机器上能真实触发；
 *   b) 单块上限（请求 > 2^BUDDY_MAX_ORDER 页）—— 本机可用页数约 3.2 万，
 *      远小于 2^19 页，"超上限"的请求一定同时超 nr_free，走不到这一支。
 * 因此 (b) 改成直接调用 buddy_order_for_request() 断言它的判定结果，等于把
 * 这条分支从 alloc 里单独提出来测：2^19 页合法，(2^19)+1 页必须判为超限。 */
static void buddy_limit_check(void) {
    unsigned int free_before = buddy_area.nr_free;
    size_t max_block = buddy_pages_of_order(BUDDY_MAX_ORDER);
    bool too_large = 0;

    /* a) 超过全部空闲页 */
    assert(alloc_pages(npage - nbase) == NULL);
    assert(alloc_pages(buddy_area.nr_free + 1) == NULL);

    /* b) 单块上限：阶计算的边界 */
    assert(buddy_order_for_request(1, &too_large) == 0 && !too_large);
    assert(buddy_order_for_request(3, &too_large) == 2 && !too_large);
    assert(buddy_order_for_request(max_block, &too_large) == BUDDY_MAX_ORDER);
    assert(!too_large);                              /* 正好 2^19 页仍然合法 */
    assert(buddy_order_for_request(max_block + 1, &too_large) == BUDDY_MAX_ORDER);
    assert(too_large);                               /* 多 1 页就必须拒绝 */

    /* 公共接口对超上限请求同样返回 NULL，且状态不变 */
    assert(alloc_pages(max_block + 1) == NULL);
    assert(buddy_area.nr_free == free_before);
    assert(buddy_sum_free_pages() == buddy_area.nr_free);
    cprintf("  [buddy] over-size request rejected ...... OK\n");
}

/* 测试 5：批量分配直到没有合适的块，再全部释放，验证耗尽路径与恢复 */
static void buddy_exhaust_check(void) {
    static struct Page *chunks[BUDDY_TEST_MAX_CHUNKS];
    unsigned int free_before = buddy_area.nr_free;
    size_t cnt = 0;

    while (cnt < BUDDY_TEST_MAX_CHUNKS) {
        struct Page *p = alloc_pages(BUDDY_TEST_CHUNK_PAGES);
        if (p == NULL) {
            break;
        }
        chunks[cnt++] = p;
    }
    assert(cnt > 0);
    assert(buddy_area.nr_free == free_before - cnt * BUDDY_TEST_CHUNK_PAGES);
    assert(buddy_sum_free_pages() == buddy_area.nr_free);

    while (cnt > 0) {
        cnt--;
        free_pages(chunks[cnt], BUDDY_TEST_CHUNK_PAGES);
    }
    assert(buddy_area.nr_free == free_before);
    assert(buddy_sum_free_pages() == buddy_area.nr_free);
    cprintf("  [buddy] bulk alloc/free until exhausted  OK\n");
}

static void buddy_check(void) {
    cprintf("[buddy test] buddy_system_pmm_manager self-test, free pages = %lu\n",
            (unsigned long)buddy_area.nr_free);
    assert(buddy_sum_free_pages() == buddy_area.nr_free);

    buddy_basic_check();
    buddy_order_check();
    buddy_split_merge_check();
    buddy_limit_check();
    buddy_exhaust_check();

    assert(buddy_sum_free_pages() == buddy_area.nr_free);
    cprintf("[buddy test] all buddy checks passed\n");
}

const struct pmm_manager buddy_system_pmm_manager = {
    .name = "buddy_system_pmm_manager",
    .init = buddy_init,
    .init_memmap = buddy_init_memmap,
    .alloc_pages = buddy_alloc_pages,
    .free_pages = buddy_free_pages,
    .nr_free_pages = buddy_nr_free_pages,
    .check = buddy_check,
};
