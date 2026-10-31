//lab2 challenge-slub 2511203
/*
 * SLUB 风格的两层内存分配器（简化实现）。
 *
 * 第一层：slab，一次向物理内存管理器申请 SLUB_SLAB_PAGES 个连续页；
 * 第二层：在 slab 内部按固定档位切分对象，用空闲链表串起未分配对象，
 *         空闲对象的摘取和插入为常数时间；查找 slab、识别释放归属仍需扫描链表。
 *
 * 布局约定（每个 slab 一页）：
 *   [0, 64)             struct slub_slab 元数据
 *   [64, 4096)          对象区，可容纳 (4096-64)/object_size 个对象
 * 空闲对象的前 8 字节（sizeof(void *)）存放"下一个空闲对象地址"，
 * 因此最小档位必须 >= 8 字节。
 *
 * 超过 SLUB_MAX_OBJECT 的请求走直通路径：直接向第一层要
 * ceil((size+16)/PGSIZE) 页，页首 16 字节写 magic 与页数，返回其后的地址。
 *
 * 空的 slab 立即销毁并把页归还，保证"全部释放后页数回到初始值"，
 * 这样泄漏就能用 nr_free_pages() 对账。
 */

#include <assert.h>
#include <defs.h>
#include <memlayout.h>
#include <mmu.h>
#include <pmm.h>
#include <stdio.h>
#include <string.h>

#include <slub_pmm.h>

#define SLUB_NUM_CLASSES 9
#define SLUB_MAX_OBJECT 2048
#define SLUB_SLAB_PAGES 1
#define SLUB_SLAB_SIZE (SLUB_SLAB_PAGES * PGSIZE)
#define SLUB_OBJ_OFFSET 64
#define SLUB_BIG_HEADER 16

#define SLUB_CANARY 0x5A5AA5A5C3C3DEDEUL
#define SLUB_BIG_MAGIC 0xB16B16B16B16B16BUL

/* 每个 slab 的元数据，放在 slab 首页的最开头 */
struct slub_slab {
    struct slub_slab *next;      // 同一个 cache 内的 slab 链
    void *page;                  // slab 首页虚拟地址（页对齐）
    unsigned int class_index;    // 属于哪个档位
    unsigned int inuse;          // 已分配对象数
    void *freelist;              // 空闲对象单链
    unsigned long canary;        // 元数据哨兵，供自测检查结构未被踩坏
};

/* 一个大小档位对应一个 cache */
struct kmem_cache {
    size_t object_size;
    unsigned int objects_per_slab;
    unsigned int slab_count;
    unsigned int inuse;
    struct slub_slab *slabs;
};

static const size_t slub_class_size[SLUB_NUM_CLASSES] = {
    8, 16, 32, 64, 128, 256, 512, 1024, 2048
};

static struct kmem_cache slub_caches[SLUB_NUM_CLASSES];
static unsigned long slub_rand_state = 12345;
static int slub_inited = 0;

/* 编译期检查：元数据必须能塞进对象区之前的保留区（用 typedef 避免 -Wunused 警告） */
typedef char slub_layout_check_t[(sizeof(struct slub_slab) <= SLUB_OBJ_OFFSET) ? 1 : -1];

/* ---------------- 小工具 ---------------- */

static void *slub_page_kva(struct Page *page) {
    return (void *)(page2pa(page) + va_pa_offset);
}

static struct Page *slub_kva_page(void *kva) {
    return pa2page(PADDR(kva));
}

static int slub_class_of(size_t size) {
    for (int i = 0; i < SLUB_NUM_CLASSES; i++) {
        if (size <= slub_class_size[i]) {
            return i;
        }
    }
    return -1;   // 超过最大档位，走直通路径
}

static unsigned long slub_next_rand(void) {
    slub_rand_state = slub_rand_state * 6364136223846793005UL
                    + 1442695040888963407UL;
    return slub_rand_state;
}

/* ---------------- 第一层：slab 的建立与销毁 ---------------- */

static struct slub_slab *slub_new_slab(struct kmem_cache *cache,
                                       unsigned int class_index) {
    struct Page *page = alloc_pages(SLUB_SLAB_PAGES);
    if (page == NULL) {
        return NULL;
    }

    char *base = (char *)slub_page_kva(page);
    struct slub_slab *slab = (struct slub_slab *)base;

    slab->page = base;
    slab->class_index = class_index;
    slab->inuse = 0;
    slab->canary = SLUB_CANARY;
    slab->freelist = NULL;

    /* 把对象区按 object_size 切开，逐个从低到高压入空闲链表 */
    char *first = base + SLUB_OBJ_OFFSET;
    for (unsigned int i = 0; i < cache->objects_per_slab; i++) {
        char *obj = first + (size_t)i * cache->object_size;
        *(void **)obj = slab->freelist;
        slab->freelist = obj;
    }

    slab->next = cache->slabs;
    cache->slabs = slab;
    cache->slab_count++;

    return slab;
}

static void slub_destroy_slab(struct kmem_cache *cache,
                              struct slub_slab *slab) {
    struct slub_slab **link = &cache->slabs;
    while (*link != NULL && *link != slab) {
        link = &(*link)->next;
    }
    assert(*link == slab);
    *link = slab->next;

    cache->slab_count--;

    struct Page *page = slub_kva_page(slab->page);
    slab->next = NULL;
    slab->freelist = NULL;
    slab->inuse = 0;

    free_pages(page, SLUB_SLAB_PAGES);
}

/* ---------------- 直通路径：大对象 ---------------- */

static void *slub_alloc_big(size_t size) {
    /* Reject overflow before adding the header and rounding to whole pages. */
    if (size > (size_t)-1 - SLUB_BIG_HEADER - (PGSIZE - 1)) {
        return NULL;
    }
    size_t need = size + SLUB_BIG_HEADER;
    size_t pages = (need + PGSIZE - 1) / PGSIZE;

    struct Page *page = alloc_pages(pages);
    if (page == NULL) {
        return NULL;
    }

    unsigned long *header = (unsigned long *)slub_page_kva(page);
    header[0] = SLUB_BIG_MAGIC;
    header[1] = (unsigned long)pages;

    return (void *)((char *)header + SLUB_BIG_HEADER);
}

/* ---------------- 对外接口 ---------------- */

void slub_init(void) {
    for (int i = 0; i < SLUB_NUM_CLASSES; i++) {
        struct kmem_cache *cache = &slub_caches[i];
        cache->object_size = slub_class_size[i];
        cache->objects_per_slab =
            (unsigned int)((SLUB_SLAB_SIZE - SLUB_OBJ_OFFSET) / cache->object_size);
        cache->slab_count = 0;
        cache->inuse = 0;
        cache->slabs = NULL;
    }
    slub_inited = 1;
}

void *slub_alloc(size_t size) {
    if (!slub_inited) {
        slub_init();
    }
    if (size == 0) {
        return NULL;
    }

    int index = slub_class_of(size);
    if (index < 0) {
        return slub_alloc_big(size);
    }

    struct kmem_cache *cache = &slub_caches[index];

    /* 优先用已有的空闲对象；没有就新开一个 slab */
    struct slub_slab *slab = cache->slabs;
    while (slab != NULL && slab->freelist == NULL) {
        slab = slab->next;
    }
    if (slab == NULL) {
        slab = slub_new_slab(cache, (unsigned int)index);
        if (slab == NULL) {
            return NULL;   // 第一层没页可给了
        }
    }

    void *obj = slab->freelist;
    slab->freelist = *(void **)obj;
    slab->inuse++;
    cache->inuse++;

    return obj;
}

void slub_free(void *ptr) {
    if (ptr == NULL) {
        return;
    }

    uintptr_t addr = (uintptr_t)ptr;

    /* 先判断是否落在某个 slab 的对象区里 */
    for (int i = 0; i < SLUB_NUM_CLASSES; i++) {
        struct kmem_cache *cache = &slub_caches[i];
        for (struct slub_slab *slab = cache->slabs; slab != NULL;
             slab = slab->next) {
            uintptr_t base = (uintptr_t)slab->page;
            if (addr >= base + SLUB_OBJ_OFFSET && addr < base + SLUB_SLAB_SIZE) {
                assert(slab->canary == SLUB_CANARY);
                assert((addr - base - SLUB_OBJ_OFFSET) % cache->object_size == 0);
                assert(slab->inuse > 0);

                *(void **)ptr = slab->freelist;
                slab->freelist = ptr;
                slab->inuse--;
                cache->inuse--;

                if (slab->inuse == 0) {
                    slub_destroy_slab(cache, slab);
                }
                return;
            }
        }
    }

    /* 不在任何 slab 里，只能是直通路径分配的大对象 */
    unsigned long *header = (unsigned long *)((char *)ptr - SLUB_BIG_HEADER);
    assert(header[0] == SLUB_BIG_MAGIC);

    size_t pages = (size_t)header[1];
    assert(pages > 0);
    header[0] = 0;

    free_pages(slub_kva_page((void *)header), pages);
}

/* ---------------- 统计接口（供测试与报告使用） ---------------- */

size_t slub_nr_slabs(void) {
    size_t total = 0;
    for (int i = 0; i < SLUB_NUM_CLASSES; i++) {
        total += slub_caches[i].slab_count;
    }
    return total;
}

size_t slub_nr_total_objects(void) {
    size_t total = 0;
    for (int i = 0; i < SLUB_NUM_CLASSES; i++) {
        total += (size_t)slub_caches[i].slab_count *
                 slub_caches[i].objects_per_slab;
    }
    return total;
}

size_t slub_nr_inuse_objects(void) {
    size_t total = 0;
    for (int i = 0; i < SLUB_NUM_CLASSES; i++) {
        total += slub_caches[i].inuse;
    }
    return total;
}

/* 元数据与空闲链自检：结构完好、空闲对象都落在对象区内 */
static int slub_structure_ok(void) {
    for (int i = 0; i < SLUB_NUM_CLASSES; i++) {
        struct kmem_cache *cache = &slub_caches[i];
        for (struct slub_slab *slab = cache->slabs; slab != NULL;
             slab = slab->next) {
            if (slab->canary != SLUB_CANARY || slab->page == NULL ||
                slab->class_index != (unsigned int)i ||
                slab->inuse > cache->objects_per_slab) {
                return 0;
            }
            uintptr_t base = (uintptr_t)slab->page;
            unsigned int nfree = 0;
            for (void *obj = slab->freelist; obj != NULL; obj = *(void **)obj) {
                uintptr_t addr = (uintptr_t)obj;
                if (addr < base + SLUB_OBJ_OFFSET ||
                    addr >= base + SLUB_SLAB_SIZE) {
                    return 0;
                }
                if ((addr - base - SLUB_OBJ_OFFSET) % cache->object_size != 0) {
                    return 0;
                }
                if (++nfree > cache->objects_per_slab) {
                    return 0;   // 空闲链表成环
                }
            }
            if (nfree + slab->inuse != cache->objects_per_slab) {
                return 0;       // 空闲 + 已分配必须恰好等于每页对象数
            }
        }
    }
    return 1;
}

/* ---------------- 九个测试用例 ---------------- */

/* 1. 基本分配：同档位对象来自同一 slab、互不重叠、释放后页数还原 */
static void slub_basic_check(void) {
    size_t free_before = nr_free_pages();

    char *a = slub_alloc(8);
    char *b = slub_alloc(8);
    char *c = slub_alloc(8);
    assert(a != NULL && b != NULL && c != NULL);
    assert(a != b && a != c && b != c);

    /* 8 字节档位第一次分配只应该开一个 slab */
    assert(slub_nr_slabs() == 1);
    assert(slub_nr_inuse_objects() == 3);

    /* 三个对象都落在同一页里，且地址相差 8 的整数倍 */
    assert(((uintptr_t)a >> PGSHIFT) == ((uintptr_t)b >> PGSHIFT));
    assert(((uintptr_t)b >> PGSHIFT) == ((uintptr_t)c >> PGSHIFT));
    assert(((uintptr_t)a - (uintptr_t)b) % 8 == 0);
    assert(slub_structure_ok());

    slub_free(a);
    slub_free(b);
    slub_free(c);

    /* 三个对象释放完，slab 应该被销毁、页数回到原值 */
    assert(slub_nr_slabs() == 0);
    assert(slub_nr_inuse_objects() == 0);
    assert(nr_free_pages() == free_before);

    cprintf("[slub] basic alloc/free ............ OK\n");
}

/* 2. 档位选择：每个档位都能分配，且对象地址按档位对齐 */
static void slub_size_class_check(void) {
    size_t free_before = nr_free_pages();
    const size_t sizes[SLUB_NUM_CLASSES] = {8, 16, 32, 64, 128, 256, 512, 1024, 2048};
    void *objs[SLUB_NUM_CLASSES];

    for (int i = 0; i < SLUB_NUM_CLASSES; i++) {
        objs[i] = slub_alloc(sizes[i]);
        assert(objs[i] != NULL);
        uintptr_t addr = (uintptr_t)objs[i];
        /* 对象区从 64 字节处开始，档位 <= 64 时对象天然按自身大小对齐 */
        assert(addr % 8 == 0);
        if (sizes[i] <= 64) {
            assert(addr % sizes[i] == 0);
        }
    }
    assert(slub_nr_inuse_objects() == SLUB_NUM_CLASSES);
    /* Cross the small-object/direct-page boundary without changing cache count. */
    void *big = slub_alloc(SLUB_MAX_OBJECT + 1);
    assert(big != NULL);
    assert(slub_nr_inuse_objects() == SLUB_NUM_CLASSES);
    slub_free(big);
    assert(slub_structure_ok());

    for (int i = SLUB_NUM_CLASSES - 1; i >= 0; i--) {
        slub_free(objs[i]);
    }
    assert(slub_nr_slabs() == 0);
    assert(nr_free_pages() == free_before);
    assert(slub_structure_ok());

    cprintf("[slub] size class selection ........ OK\n");
}

/* 3. slab 扩容：一个 slab 装不下时自动开第二个 */
static void slub_slab_grow_check(void) {
    size_t free_before = nr_free_pages();
    const size_t size = 64;                       // 每页 (4096-64)/64 = 63 个
    const unsigned int per_slab = 63;
    const unsigned int count = per_slab + 1;

    void *objs[64];
    for (unsigned int i = 0; i < count; i++) {
        objs[i] = slub_alloc(size);
        assert(objs[i] != NULL);
    }
    assert(slub_nr_slabs() == 2);
    assert(slub_nr_inuse_objects() == count);
    assert(nr_free_pages() == free_before - 2);
    assert(slub_structure_ok());

    for (unsigned int i = 0; i < count; i++) {
        slub_free(objs[i]);
    }
    assert(slub_nr_slabs() == 0);
    assert(nr_free_pages() == free_before);

    cprintf("[slub] slab grow on demand ......... OK\n");
}

/* 4. 释放后重用：刚释放的对象应被下一次同档位分配优先复用（LIFO） */
static void slub_free_reuse_check(void) {
    size_t free_before = nr_free_pages();

    /* 先占用两个对象，避免释放第一个就把整个 slab 销毁，
     * 这样检查的是"空闲链的 LIFO 语义"，而不是"页分配器是否巧合返回同一页" */
    void *first = slub_alloc(32);
    void *keep = slub_alloc(32);
    assert(first != NULL && keep != NULL && first != keep);

    slub_free(first);
    void *again = slub_alloc(32);
    assert(again == first);   /* 刚释放的对象必须在空闲链头部 */

    slub_free(again);
    slub_free(keep);
    assert(slub_nr_slabs() == 0);
    assert(nr_free_pages() == free_before);

    cprintf("[slub] free then reuse ............. OK\n");
}

/* 5. 泄漏检查：跨档位混合分配全部释放后，页数与对象计数必须回到初始值 */
static void slub_leak_check(void) {
    size_t free_before = nr_free_pages();
    const size_t sizes[6] = {8, 64, 256, 1024, 2048, 3000};   // 3000 走直通
    void *objs[18];

    for (int round = 0; round < 3; round++) {
        for (int i = 0; i < 6; i++) {
            objs[round * 6 + i] = slub_alloc(sizes[i]);
            assert(objs[round * 6 + i] != NULL);
        }
    }
    assert(slub_nr_inuse_objects() > 0);
    assert(nr_free_pages() < free_before);

    for (int i = 17; i >= 0; i--) {
        slub_free(objs[i]);
    }

    assert(slub_nr_slabs() == 0);
    assert(slub_nr_inuse_objects() == 0);
    assert(nr_free_pages() == free_before);

    cprintf("[slub] no leak after mixed free ..... OK\n");
}

/* 6. 越界防护：写满一个对象不得影响相邻对象；元数据 sentinel 必须完好 */
static void slub_guard_check(void) {
    char *x = slub_alloc(64);
    char *y = slub_alloc(64);
    assert(x != NULL && y != NULL);

    char *lo = x;
    char *hi = y;
    if ((uintptr_t)x > (uintptr_t)y) {
        lo = y;
        hi = x;
    }
    /* 同一 slab 内相邻的两个对象，间隔恰为一个对象大小 */
    assert((uintptr_t)hi - (uintptr_t)lo == 64);

    unsigned long saved = *(unsigned long *)hi;
    memset(lo, 0xAA, 64);
    assert(*(unsigned long *)hi == saved);   // 越界写没有波及邻居
    assert(slub_structure_ok());             // 元数据没被对象区踩坏

    slub_free(x);
    slub_free(y);
    assert(slub_nr_slabs() == 0);

    cprintf("[slub] neighbour guard ............. OK\n");
}

/* 7. 混合负载：先做一轮"固定覆盖"，再跑随机分配/释放。
 *
 * 只用随机数决定尺寸会漏覆盖：旧写法每步连续取两个随机状态、分别 `% 8` 和
 * `% 6`，而 LCG 的相邻状态在低位高度相关，尺寸与槽位被绑成两族，实测 8、
 * 600、5000 三个尺寸一次都没被选中，八个槽位也只用到 0/2/4/6。换成换种子
 * 解决不了这个结构性问题，所以改成"固定阶段保证覆盖 + 随机阶段制造交织"，
 * 并把覆盖情况用计数器断言钉死。 */
static void slub_mixed_check(void) {
    size_t free_before = nr_free_pages();
    const size_t sizes[6] = {8, 100, 600, 2049, 5000, 12};
    void *ring[8];
    size_t ring_size[8];
    unsigned int size_hit[6] = {0};   // 每个尺寸被申请的次数
    unsigned int slot_hit[8] = {0};   // 每个槽位被使用的次数
    size_t small_filled = 0;          // 走 slab 的对象数，用于与 cache 计数对账

    for (int i = 0; i < 8; i++) {
        ring[i] = NULL;
        ring_size[i] = 0;
    }

    /* 阶段 1：固定覆盖。八个槽位各取一个对象、六个尺寸轮流使用，
     * 使"每个尺寸至少申请过一次、每个槽位至少用过一次"不依赖随机数的运气。 */
    for (int i = 0; i < 8; i++) {
        int pick = i % 6;
        ring[i] = slub_alloc(sizes[pick]);
        assert(ring[i] != NULL);
        ring_size[i] = sizes[pick];
        slot_hit[i]++;
        size_hit[pick]++;
        if (sizes[pick] <= SLUB_MAX_OBJECT) {
            small_filled++;
        }
        assert(slub_structure_ok());
        assert(slub_nr_inuse_objects() == small_filled);
    }
    for (int i = 0; i < 6; i++) {
        assert(size_hit[i] >= 1);     // 六个尺寸都覆盖到了
    }
    for (int i = 0; i < 8; i++) {
        assert(slot_hit[i] >= 1);     // 八个槽位都用到了
    }

    /* 阶段 2：随机负载。每步只取一次随机数，槽位与尺寸分别取它的不同位段，
     * 避免两个连续状态之间的低位相关。 */
    for (int step = 0; step < 200; step++) {
        unsigned long r = slub_next_rand();
        int slot = (int)((r >> 13) % 8);
        int pick = (int)((r >> 37) % 6);

        if (ring[slot] != NULL) {
            /* 释放分支也要检查：结构与对象计数在释放之后同样必须成立 */
            size_t sz = ring_size[slot];
            slub_free(ring[slot]);
            ring[slot] = NULL;
            ring_size[slot] = 0;
            if (sz <= SLUB_MAX_OBJECT) {
                small_filled--;
            }
        } else {
            ring[slot] = slub_alloc(sizes[pick]);
            assert(ring[slot] != NULL);
            ring_size[slot] = sizes[pick];
            slot_hit[slot]++;
            size_hit[pick]++;
            if (sizes[pick] <= SLUB_MAX_OBJECT) {
                small_filled++;
            }
        }

        assert(slub_structure_ok());
        assert(slub_nr_inuse_objects() == small_filled);
    }

    /* 随机阶段结束：六种尺寸仍然都至少被申请过一次（固定阶段已保证，这里复验） */
    for (int i = 0; i < 6; i++) {
        assert(size_hit[i] >= 1);
    }

    for (int i = 0; i < 8; i++) {
        if (ring[i] != NULL) {
            slub_free(ring[i]);
            ring[i] = NULL;
        }
    }

    assert(slub_nr_inuse_objects() == 0);
    assert(slub_nr_slabs() == 0);
    assert(nr_free_pages() == free_before);

    cprintf("[slub] mixed random workload ....... OK\n");
}

/* 8. 边界参数：0 字节与超出物理内存的请求必须失败而不是崩溃 */
static void slub_oom_check(void) {
    size_t free_before = nr_free_pages();

    assert(slub_alloc(0) == NULL);
    slub_free(NULL);
    assert(slub_alloc((size_t)-1) == NULL);
    assert(slub_alloc((size_t)-SLUB_BIG_HEADER) == NULL);

    /* 1 TiB 远超 128 MB 物理内存，第一层必然拒绝 */
    void *huge = slub_alloc((size_t)1 << 40);
    assert(huge == NULL);

    assert(nr_free_pages() == free_before);
    assert(slub_nr_inuse_objects() == 0);

    cprintf("[slub] invalid & oversized request .. OK\n");
}

/* 9. cache 重建：销毁后重新初始化，计数清零且仍能正常分配 */
static void slub_cache_reuse_check(void) {
    assert(slub_nr_slabs() == 0);
    assert(slub_nr_inuse_objects() == 0);

    slub_init();
    assert(slub_nr_slabs() == 0);
    assert(slub_nr_total_objects() == 0);

    size_t free_before = nr_free_pages();
    void *obj = slub_alloc(2048);
    assert(obj != NULL);
    assert(slub_nr_slabs() == 1);
    assert(slub_nr_total_objects() == 1);      // 2048 档每页只切一个
    slub_free(obj);
    assert(slub_nr_slabs() == 0);
    assert(nr_free_pages() == free_before);

    cprintf("[slub] cache re-init ................ OK\n");
}

void slub_check(void) {
    cprintf("[slub test] slub_pmm self-test, free pages = %u\n",
            (unsigned int)nr_free_pages());

    slub_basic_check();
    slub_size_class_check();
    slub_slab_grow_check();
    slub_free_reuse_check();
    slub_leak_check();
    slub_guard_check();
    slub_mixed_check();
    slub_oom_check();
    slub_cache_reuse_check();

    assert(slub_nr_slabs() == 0);
    assert(slub_nr_inuse_objects() == 0);
    assert(slub_structure_ok());

    cprintf("[slub test] all slub checks passed\n");
}
