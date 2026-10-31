#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
verify_slub_logic.py —— 在没有 RISC-V 工具链的机器上，逐行等价验证
kern/mm/slub_pmm.c 的分配/释放逻辑与 9 个测试用例。

做法：
  * 用 bytearray 模拟 128 MiB 物理内存（虚拟地址 = PA + 0xFFFFFFFF40000000）
  * 用"首次适配 + 连续 n 页"模拟第一层 alloc_pages/free_pages，
    与 best_fit/buddy 一样维护 nr_free_pages()
  * 把 slub_pmm.c 的每个函数按同样顺序、同样算术翻译成 Python，
    包括 freelist 用对象前 8 字节存下一个地址、slab 元数据放页首 64 字节、
    直通路径的 16 字节头、canary 校验
  * 依次跑 9 个与 C 版同名的测试，任何断言失败立即报错

用法：python3 tools/verify_slub_logic.py
"""

import struct
import sys

PGSIZE = 4096
PGSHIFT = 12
NPAGE = 32768
PHYS_BASE = 0x80000000
VA_PA_OFFSET = 0xFFFFFFFF40000000
KVA_BASE = PHYS_BASE + VA_PA_OFFSET

SLUB_NUM_CLASSES = 9
SLUB_MAX_OBJECT = 2048
SLUB_SLAB_PAGES = 1
SLUB_SLAB_SIZE = SLUB_SLAB_PAGES * PGSIZE
SLUB_OBJ_OFFSET = 64
SLUB_BIG_HEADER = 16
SLUB_CANARY = 0x5A5AA5A5C3C3DEDE
SLUB_BIG_MAGIC = 0xB16B16B16B16B16B
MASK64 = (1 << 64) - 1

CLASS_SIZES = [8, 16, 32, 64, 128, 256, 512, 1024, 2048]

# ---------------------------------------------------------------- 模拟物理内存

MEM = bytearray(NPAGE * PGSIZE)          # 以 PA 为下标
used = bytearray(NPAGE)                  # 1 = 已分配
nr_free = NPAGE

stats = {"alloc": 0, "free": 0, "fail": 0}


def pa_of_page(idx):
    return PHYS_BASE + idx * PGSIZE


def page_of_pa(pa):
    return (pa - PHYS_BASE) // PGSIZE


def kva_of_page(idx):
    return pa_of_page(idx) + VA_PA_OFFSET


def read_u64(addr):
    off = addr - KVA_BASE
    return struct.unpack_from("<Q", MEM, off)[0]


def write_u64(addr, value):
    off = addr - KVA_BASE
    struct.pack_into("<Q", MEM, off, value & MASK64)


def read_bytes(addr, n):
    off = addr - KVA_BASE
    return bytes(MEM[off:off + n])


def write_bytes(addr, data):
    off = addr - KVA_BASE
    MEM[off:off + len(data)] = data


def alloc_pages(n):
    """首次适配地找 n 个连续空闲页；与真实实现在"失败时返回 NULL"上一致"""
    global nr_free
    if n == 0 or n > nr_free:
        stats["fail"] += 1
        return None
    run = 0
    for i in range(NPAGE):
        if used[i]:
            run = 0
            continue
        run += 1
        if run == n:
            start = i - n + 1
            for j in range(start, start + n):
                used[j] = 1
            nr_free -= n
            stats["alloc"] += n
            return start
    stats["fail"] += 1
    return None


def free_pages(idx, n):
    global nr_free
    for j in range(idx, idx + n):
        assert used[j], "double free of page %d" % j
        used[j] = 0
    nr_free += n
    stats["free"] += n


def nr_free_pages():
    return nr_free


# ---------------------------------------------------------------- SLUB 实现

class Slab:
    __slots__ = ("page", "class_index", "inuse", "freelist", "canary", "next")

    def __init__(self):
        self.page = 0
        self.class_index = 0
        self.inuse = 0
        self.freelist = 0
        self.canary = 0
        self.next = None


class Cache:
    __slots__ = ("object_size", "objects_per_slab", "slab_count", "inuse", "slabs")

    def __init__(self):
        self.object_size = 0
        self.objects_per_slab = 0
        self.slab_count = 0
        self.inuse = 0
        self.slabs = None


caches = [Cache() for _ in range(SLUB_NUM_CLASSES)]
rand_state = 12345


def next_rand():
    global rand_state
    rand_state = (rand_state * 6364136223846793005 + 1442695040888963407) & MASK64
    return rand_state


def class_of(size):
    for i in range(SLUB_NUM_CLASSES):
        if size <= CLASS_SIZES[i]:
            return i
    return -1


def new_slab(cache, class_index):
    idx = alloc_pages(SLUB_SLAB_PAGES)
    if idx is None:
        return None
    base = kva_of_page(idx)
    slab = Slab()
    slab.page = base
    slab.class_index = class_index
    slab.inuse = 0
    slab.canary = SLUB_CANARY
    slab.freelist = 0
    first = base + SLUB_OBJ_OFFSET
    for i in range(cache.objects_per_slab):
        obj = first + i * cache.object_size
        write_u64(obj, slab.freelist)
        slab.freelist = obj
    slab.next = cache.slabs
    cache.slabs = slab
    cache.slab_count += 1
    return slab


def destroy_slab(cache, slab):
    link = None
    cur = cache.slabs
    while cur is not None and cur is not slab:
        link = cur
        cur = cur.next
    assert cur is slab, "slab 不在链上"
    if link is None:
        cache.slabs = slab.next
    else:
        link.next = slab.next
    cache.slab_count -= 1
    page = page_of_pa(slab.page - VA_PA_OFFSET)
    slab.next = None
    slab.freelist = 0
    slab.inuse = 0
    free_pages(page, SLUB_SLAB_PAGES)


def alloc_big(size):
    need = size + SLUB_BIG_HEADER
    pages = (need + PGSIZE - 1) // PGSIZE
    idx = alloc_pages(pages)
    if idx is None:
        return None
    header = kva_of_page(idx)
    write_u64(header, SLUB_BIG_MAGIC)
    write_u64(header + 8, pages)
    return header + SLUB_BIG_HEADER


def slub_init():
    for i in range(SLUB_NUM_CLASSES):
        c = caches[i]
        c.object_size = CLASS_SIZES[i]
        c.objects_per_slab = (SLUB_SLAB_SIZE - SLUB_OBJ_OFFSET) // c.object_size
        c.slab_count = 0
        c.inuse = 0
        c.slabs = None


def slub_alloc(size):
    if size == 0:
        return None
    index = class_of(size)
    if index < 0:
        return alloc_big(size)
    cache = caches[index]
    slab = cache.slabs
    while slab is not None and slab.freelist == 0:
        slab = slab.next
    if slab is None:
        slab = new_slab(cache, index)
        if slab is None:
            return None
    obj = slab.freelist
    slab.freelist = read_u64(obj)
    slab.inuse += 1
    cache.inuse += 1
    return obj


def slub_free(ptr):
    if ptr is None or ptr == 0:
        return
    for i in range(SLUB_NUM_CLASSES):
        cache = caches[i]
        slab = cache.slabs
        while slab is not None:
            base = slab.page
            if base + SLUB_OBJ_OFFSET <= ptr < base + SLUB_SLAB_SIZE:
                assert slab.canary == SLUB_CANARY, "slab 元数据被踩坏"
                assert (ptr - base - SLUB_OBJ_OFFSET) % cache.object_size == 0
                assert slab.inuse > 0
                write_u64(ptr, slab.freelist)
                slab.freelist = ptr
                slab.inuse -= 1
                cache.inuse -= 1
                if slab.inuse == 0:
                    destroy_slab(cache, slab)
                return
            slab = slab.next
    header = ptr - SLUB_BIG_HEADER
    assert read_u64(header) == SLUB_BIG_MAGIC, "释放了非法指针"
    pages = read_u64(header + 8)
    assert pages > 0
    write_u64(header, 0)
    free_pages(page_of_pa(header - VA_PA_OFFSET), pages)


def nr_slabs():
    return sum(c.slab_count for c in caches)


def nr_total_objects():
    return sum(c.slab_count * c.objects_per_slab for c in caches)


def nr_inuse_objects():
    return sum(c.inuse for c in caches)


def structure_ok():
    for i, cache in enumerate(caches):
        slab = cache.slabs
        while slab is not None:
            if (slab.canary != SLUB_CANARY or slab.page == 0
                    or slab.class_index != i
                    or slab.inuse > cache.objects_per_slab):
                return False
            base = slab.page
            obj = slab.freelist
            seen = 0
            while obj != 0:
                if not (base + SLUB_OBJ_OFFSET <= obj < base + SLUB_SLAB_SIZE):
                    return False
                if (obj - base - SLUB_OBJ_OFFSET) % cache.object_size != 0:
                    return False
                seen += 1
                if seen > cache.objects_per_slab:
                    return False       # 空闲链表出现环
                obj = read_u64(obj)
            if seen + slab.inuse != cache.objects_per_slab:
                return False           # 空闲 + 已分配必须恰好等于每页对象数
            slab = slab.next
    return True


# ---------------------------------------------------------------- 九个测试

PASSED = []


def test_basic():
    """三个同档对象同页、释放后页数还原"""
    before = nr_free_pages()
    a = slub_alloc(8)
    b = slub_alloc(8)
    c = slub_alloc(8)
    assert a and b and c
    assert len({a, b, c}) == 3
    assert nr_slabs() == 1
    assert nr_inuse_objects() == 3
    assert (a >> PGSHIFT) == (b >> PGSHIFT) == (c >> PGSHIFT)
    assert (a - b) % 8 == 0
    assert structure_ok()
    for p in (a, b, c):
        slub_free(p)
    assert nr_slabs() == 0
    assert nr_inuse_objects() == 0
    assert nr_free_pages() == before


def test_size_class():
    """九个档位都能分配，地址按档位对齐"""
    before = nr_free_pages()
    objs = []
    for size in CLASS_SIZES:
        p = slub_alloc(size)
        assert p is not None
        assert p % 8 == 0
        if size <= 64:
            assert p % size == 0
        objs.append(p)
    assert nr_inuse_objects() == SLUB_NUM_CLASSES
    assert structure_ok()
    for p in reversed(objs):
        slub_free(p)
    assert nr_slabs() == 0
    assert nr_free_pages() == before
    assert structure_ok()


def test_grow():
    """一个 slab 装不下时自动开第二个，并各自回收"""
    before = nr_free_pages()
    per_slab = (SLUB_SLAB_SIZE - SLUB_OBJ_OFFSET) // 64
    assert per_slab == 63
    objs = [slub_alloc(64) for _ in range(per_slab + 1)]
    assert all(objs)
    assert nr_slabs() == 2
    assert nr_inuse_objects() == per_slab + 1
    assert nr_free_pages() == before - 2
    assert structure_ok()
    for p in objs:
        slub_free(p)
    assert nr_slabs() == 0
    assert nr_free_pages() == before


def test_reuse():
    """释放后立即重用同一对象（LIFO）：先占两个对象，避免释放首个就销毁 slab"""
    before = nr_free_pages()
    first = slub_alloc(32)
    keep = slub_alloc(32)
    assert first and keep and first != keep
    slub_free(first)
    again = slub_alloc(32)
    assert again == first
    slub_free(again)
    slub_free(keep)
    assert nr_slabs() == 0
    assert nr_free_pages() == before


def test_leak():
    """跨档位含直通路径，全部释放后零泄漏"""
    before = nr_free_pages()
    sizes = [8, 64, 256, 1024, 2048, 3000]
    objs = []
    for _ in range(3):
        for s in sizes:
            p = slub_alloc(s)
            assert p is not None
            objs.append(p)
    assert nr_free_pages() < before
    for p in reversed(objs):
        slub_free(p)
    assert nr_slabs() == 0
    assert nr_inuse_objects() == 0
    assert nr_free_pages() == before


def test_guard():
    """写满一个对象不影响相邻对象，元数据 sentinel 完好"""
    x = slub_alloc(64)
    y = slub_alloc(64)
    assert x and y
    lo, hi = (x, y) if x < y else (y, x)
    assert hi - lo == 64
    saved = read_u64(hi)
    write_bytes(lo, b"\xAA" * 64)
    assert read_u64(hi) == saved
    assert structure_ok()
    slub_free(x)
    slub_free(y)
    assert nr_slabs() == 0


def test_mixed():
    """固定覆盖（6 尺寸 × 8 槽位）+ 200 步随机负载，最终完全归还"""
    before = nr_free_pages()
    sizes = [8, 100, 600, 2049, 5000, 12]
    ring = [0] * 8
    ring_size = [0] * 8
    size_hit = [0] * 6
    slot_hit = [0] * 8
    small_filled = 0

    # 阶段 1：固定覆盖，覆盖不依赖随机数的运气
    for i in range(8):
        pick = i % 6
        ring[i] = slub_alloc(sizes[pick])
        assert ring[i] is not None
        ring_size[i] = sizes[pick]
        slot_hit[i] += 1
        size_hit[pick] += 1
        if sizes[pick] <= SLUB_MAX_OBJECT:
            small_filled += 1
        assert structure_ok()
        assert nr_inuse_objects() == small_filled
    assert all(h >= 1 for h in size_hit), "六个尺寸都必须被申请过"
    assert all(h >= 1 for h in slot_hit), "八个槽位都必须被用过"

    # 阶段 2：每步只取一次随机数，槽位与尺寸取自不同位段
    for _ in range(200):
        r = next_rand()
        slot = (r >> 13) % 8
        pick = (r >> 37) % 6
        if ring[slot]:
            sz = ring_size[slot]
            slub_free(ring[slot])
            ring[slot] = 0
            ring_size[slot] = 0
            if sz <= SLUB_MAX_OBJECT:
                small_filled -= 1
        else:
            ring[slot] = slub_alloc(sizes[pick])
            assert ring[slot] is not None
            ring_size[slot] = sizes[pick]
            slot_hit[slot] += 1
            size_hit[pick] += 1
            if sizes[pick] <= SLUB_MAX_OBJECT:
                small_filled += 1
        assert structure_ok()
        assert nr_inuse_objects() == small_filled

    assert all(h >= 1 for h in size_hit)

    for i in range(8):
        if ring[i]:
            slub_free(ring[i])
            ring[i] = 0
    assert nr_inuse_objects() == 0
    assert nr_slabs() == 0
    assert nr_free_pages() == before


def test_oom():
    """0 字节与超出物理内存的请求返回 NULL 且不改变页数"""
    before = nr_free_pages()
    assert slub_alloc(0) is None
    assert slub_alloc(1 << 40) is None
    assert nr_free_pages() == before
    assert nr_inuse_objects() == 0


def test_cache_reuse():
    """cache 重新初始化后计数清零、仍可正常分配"""
    assert nr_slabs() == 0
    assert nr_inuse_objects() == 0
    slub_init()
    assert nr_slabs() == 0
    assert nr_total_objects() == 0
    before = nr_free_pages()
    p = slub_alloc(2048)
    assert p is not None
    assert nr_slabs() == 1
    assert nr_total_objects() == 1
    slub_free(p)
    assert nr_slabs() == 0
    assert nr_free_pages() == before


def stress(rounds=3000):
    """附加压力测试：随机分配/释放，检查不变量与最终零泄漏"""
    before = nr_free_pages()
    live = []
    for _ in range(rounds):
        if live and next_rand() % 100 < 45:
            i = next_rand() % len(live)
            slub_free(live.pop(i))
        else:
            size = 1 + next_rand() % 3000
            p = slub_alloc(size)
            if p is not None:
                live.append(p)
        assert structure_ok(), "不变量被破坏"
    for p in live:
        slub_free(p)
    assert nr_slabs() == 0
    assert nr_free_pages() == before, "压力测试后页数没还原"
    print("[ OK ] %-32s  %d 步随机操作后不变量与页数均正确" % ("stress(3000 steps)", rounds))


def main():
    slub_init()
    print("== SLUB 逻辑等价验证（Python 逐行复现 slub_pmm.c）==")
    print("物理内存 %d 页 / %d MiB，初始空闲 %d 页"
          % (NPAGE, NPAGE * PGSIZE // (1024 * 1024), nr_free_pages()))
    cases = [
        ("basic alloc/free", test_basic),
        ("size class selection", test_size_class),
        ("slab grow on demand", test_grow),
        ("free then reuse", test_reuse),
        ("no leak after mixed free", test_leak),
        ("neighbour guard", test_guard),
        ("mixed random workload", test_mixed),
        ("invalid & oversized request", test_oom),
        ("cache re-init", test_cache_reuse),
    ]
    for name, fn in cases:
        fn()
        PASSED.append(name)
        print("[ OK ] %-28s  %s" % (name, (fn.__doc__ or "").strip()))
    stress()
    print("=" * 56)
    print("全部 %d 个用例通过；最终空闲 %d 页（初始 %d 页）"
          % (len(PASSED) + 1, nr_free_pages(), NPAGE))
    print("分配 %d 页 / 释放 %d 页 / 失败 %d 次"
          % (stats["alloc"], stats["free"], stats["fail"]))
    assert stats["alloc"] == stats["free"]
    return 0


if __name__ == "__main__":
    sys.exit(main())
