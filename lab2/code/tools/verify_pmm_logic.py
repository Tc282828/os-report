"""对 kern/mm/best_fit_pmm.c 与 kern/mm/buddy_system_pmm.c 的逻辑做等价模拟验证。

由于本机没有 RISC-V 交叉工具链，这里把 C 代码逐行翻译成 Python，
用课程测试函数 best_fit_check / basic_check 以及 Buddy 自带的 5 个测试
在多种“内核结束地址”下反复跑，确认逻辑（而不只是语法）是正确的。
"""

NPAGE = 0x88000      # npage  = PPN(0x88000000)
NBASE = 0x80000      # nbase  = DRAM_BASE / PGSIZE


class PF:
    __slots__ = ("ref", "reserved", "property", "in_list")

    def __init__(self):
        self.ref = 0
        self.reserved = False
        self.property = 0
        self.in_list = False


class Alloc:
    """两种分配器共用的 Page 数组与断言工具"""

    def __init__(self, first_free_idx, n):
        self.P = [PF() for _ in range(NPAGE - NBASE)]
        # page_init: 先把 [nbase, npage) 全部置 Reserved
        for p in self.P:
            p.reserved = True
        self.first = first_free_idx      # pages 数组之后的第一个完整页（相对下标）
        self.n = n
        self.calls = 0

    # ---- 断言：失败即抛异常，与内核 panic 等价 ----
    def check(self, cond, msg):
        self.calls += 1
        if not cond:
            raise AssertionError(msg)


class BestFit(Alloc):
    def __init__(self, first_free_idx, n):
        super().__init__(first_free_idx, n)
        self.free_list = []
        self.nr_free = 0
        self.init_memmap(self.first, self.n)

    def insert(self, idx):
        self.free_list.append(idx)
        self.free_list.sort()
        self.P[idx].in_list = True

    def init_memmap(self, base, n):
        if n <= 0:
            raise AssertionError("init_memmap n<=0")
        for i in range(base, base + n):
            self.check(self.P[i].reserved, "init_memmap: page not reserved")
            self.P[i].property = 0
            self.P[i].ref = 0
            self.P[i].reserved = False   # C 里只清 flags，这里用 reserved 表示 flags 位
        self.P[base].property = n
        self.nr_free += n
        self.insert(base)

    def alloc_pages(self, n):
        if n <= 0:
            raise AssertionError("alloc n<=0")
        if n > self.nr_free:
            return None
        best = None
        for i in self.free_list:
            prop = self.P[i].property
            if prop >= n and (best is None or prop < self.P[best].property):
                best = i
                if self.P[best].property == n:
                    break
        if best is None:
            return None
        old = self.P[best].property
        self.free_list.remove(best)
        self.P[best].in_list = False
        if old > n:
            tail = best + n
            self.P[tail].property = old - n
            self.insert(tail)
        self.nr_free -= n
        return best       # C 里分配的块头不再有 PageProperty 位

    def is_free_head(self, idx):
        return self.P[idx].in_list and not self.P[idx].reserved

    def free_pages(self, base, n):
        if n <= 0:
            raise AssertionError("free n<=0")
        for i in range(base, base + n):
            self.check(not self.P[i].reserved and not self.is_free_head(i),
                       "free_pages: page %d already free" % i)
            self.P[i].property = 0
            self.P[i].ref = 0
        self.P[base].property = n
        self.nr_free += n
        self.insert(base)

        pos = self.free_list.index(base)
        if pos > 0:
            prev = self.free_list[pos - 1]
            if prev + self.P[prev].property == base:
                self.P[prev].property += self.P[base].property
                self.free_list.remove(base)
                self.P[base].in_list = False
                base = prev

        pos = self.free_list.index(base)
        if pos + 1 < len(self.free_list):
            nxt = self.free_list[pos + 1]
            if base + self.P[base].property == nxt:
                self.P[base].property += self.P[nxt].property
                self.free_list.remove(nxt)
                self.P[nxt].in_list = False

    def nr_free_pages(self):
        return self.nr_free


def run_best_fit_check(sim):
    """best_fit_pmm.c 中的 best_fit_check() + basic_check() 的等价复现"""
    c = sim.check
    # 头部一致性检查
    total = sum(sim.P[i].property for i in sim.free_list)
    c(total == sim.nr_free_pages(), "total != nr_free_pages")

    # ---- basic_check ----
    free_store = list(sim.free_list)
    p0 = sim.alloc_pages(1)
    p1 = sim.alloc_pages(1)
    p2 = sim.alloc_pages(1)
    c(p0 is not None and p1 is not None and p2 is not None, "basic: alloc NULL")
    c(len({p0, p1, p2}) == 3, "basic: dup pages")
    c(all(sim.P[i].ref == 0 for i in (p0, p1, p2)), "basic: ref != 0")

    store_list = list(sim.free_list)
    sim.free_list = []
    for i in store_list:
        sim.P[i].in_list = False
    store_nr = sim.nr_free
    sim.nr_free = 0

    c(sim.alloc_pages(1) is None, "basic: alloc on empty list")

    sim.free_pages(p0, 1)
    sim.free_pages(p1, 1)
    sim.free_pages(p2, 1)
    c(sim.nr_free == 3, "basic: nr_free != 3")

    p0 = sim.alloc_pages(1)
    p1 = sim.alloc_pages(1)
    p2 = sim.alloc_pages(1)
    c(p0 is not None and p1 is not None and p2 is not None, "basic: realloc NULL")
    c(sim.alloc_pages(1) is None, "basic: alloc should fail")

    sim.free_pages(p0, 1)
    c(len(sim.free_list) > 0, "basic: list empty after free")
    p = sim.alloc_pages(1)
    c(p == p0, "basic: not returning the freed page")
    c(sim.alloc_pages(1) is None, "basic: alloc should fail 2")
    c(sim.nr_free == 0, "basic: nr_free != 0")

    sim.free_list = store_list
    for i in store_list:
        sim.P[i].in_list = True
    sim.nr_free = store_nr
    sim.free_pages(p, 1)
    sim.free_pages(p1, 1)
    sim.free_pages(p2, 1)
    c(sim.free_list == free_store, "basic: list not restored")

    # ---- best_fit_check 主流程 ----
    count = len(sim.free_list)
    total = sum(sim.P[i].property for i in sim.free_list)
    c(total == sim.nr_free_pages(), "total != nr_free_pages (2)")

    p0 = sim.alloc_pages(5)
    c(p0 is not None, "p0 = alloc_pages(5) failed")
    c(not sim.is_free_head(p0), "PageProperty(p0) should be cleared")

    store_list = list(sim.free_list)
    sim.free_list = []
    for i in store_list:
        sim.P[i].in_list = False
    c(len(sim.free_list) == 0, "list_init failed")
    c(sim.alloc_pages(1) is None, "alloc on empty list should fail")

    store_nr = sim.nr_free
    sim.nr_free = 0

    # * - - * -
    sim.free_pages(p0 + 1, 2)
    sim.free_pages(p0 + 4, 1)
    c(sim.alloc_pages(4) is None, "alloc_pages(4) should fail")
    c(sim.is_free_head(p0 + 1) and sim.P[p0 + 1].property == 2,
      "p0[1].property != 2")
    # * - - * *
    p1 = sim.alloc_pages(1)
    c(p1 is not None, "alloc_pages(1) failed")
    c(sim.alloc_pages(2) is not None, "alloc_pages(2) failed")
    c(p0 + 4 == p1, "best fit should pick the 1-page block at p0+4, got p0+%d" % (p1 - p0))

    p2 = p0 + 1
    sim.free_pages(p0, 5)
    p0 = sim.alloc_pages(5)
    c(p0 is not None, "alloc_pages(5) after free failed")
    c(sim.alloc_pages(1) is None, "alloc after taking all pages should fail")
    c(sim.nr_free == 0, "nr_free != 0")

    sim.nr_free = store_nr
    sim.free_list = store_list
    for i in store_list:
        sim.P[i].in_list = True
    sim.free_pages(p0, 5)

    count2 = len(sim.free_list)
    total2 = sum(sim.P[i].property for i in sim.free_list)
    c(count2 - count == 0, "count mismatch: %d vs %d" % (count2, count))
    c(total2 - total == 0, "total mismatch")


class Buddy(Alloc):
    MAX_ORDER = 19

    def __init__(self, first_free_idx, n):
        super().__init__(first_free_idx, n)
        self.free = [[] for _ in range(self.MAX_ORDER + 1)]   # 每阶：块头下标列表（列表头在 [0]）
        self.nr_free = 0
        self.init_memmap(self.first, self.n)

    @staticmethod
    def pages_of(order):
        return 1 << order

    def order_of(self, pages):
        order = 0
        while order < self.MAX_ORDER and self.pages_of(order) < pages:
            order += 1
        return order

    def push(self, base, order):
        self.P[base].property = self.pages_of(order)
        self.P[base].in_list = True
        self.free[order].insert(0, base)      # list_add 头插

    def init_memmap(self, base, n):
        if n <= 0:
            raise AssertionError("init_memmap n<=0")
        for i in range(base, base + n):
            self.check(self.P[i].reserved, "init_memmap: not reserved")
            self.P[i].property = 0
            self.P[i].ref = 0
            self.P[i].reserved = False
        while n > 0:
            idx = base
            order = self.MAX_ORDER
            while order > 0:
                size = self.pages_of(order)
                if size <= n and idx % size == 0:
                    break
                order -= 1
            size = self.pages_of(order)
            self.push(base, order)
            self.nr_free += size
            base += size
            n -= size

    def alloc_pages(self, n):
        if n <= 0:
            raise AssertionError("alloc n<=0")
        if n > self.nr_free:
            return None
        order = self.order_of(n)
        if self.pages_of(order) < n:
            return None
        cur = order
        while cur <= self.MAX_ORDER and not self.free[cur]:
            cur += 1
        if cur > self.MAX_ORDER:
            return None
        block = self.free[cur].pop(0)
        self.P[block].in_list = False
        while cur > order:
            cur -= 1
            self.push(block + self.pages_of(cur), cur)
        self.P[block].property = self.pages_of(order)   # 记录真实块大小
        self.nr_free -= self.pages_of(order)
        return block

    def free_pages(self, base, n):
        if n <= 0:
            raise AssertionError("free n<=0")
        self.check(not self.P[base].reserved, "free_pages: reserved page")
        size = max(self.P[base].property, n)
        if size & (size - 1):
            size = self.pages_of(self.order_of(size))
        for i in range(base, base + size):
            self.check(not self.P[i].reserved, "free_pages: reserved page in block")
            self.P[i].property = 0
            self.P[i].ref = 0
        self.nr_free += size
        order = self.order_of(size)
        while order < self.MAX_ORDER:
            idx = base
            buddy = idx ^ self.pages_of(order)
            if buddy >= NPAGE - NBASE:
                break
            if not (self.P[buddy].in_list and self.P[buddy].property == self.pages_of(order)):
                break
            self.free[order].remove(buddy)
            self.P[buddy].in_list = False
            self.P[buddy].property = 0
            order += 1
            if buddy < base:
                base = buddy
        self.push(base, order)

    def nr_free_pages(self):
        return self.nr_free

    # ---- 自带测试 ----
    def sum_free_pages(self):
        c = self.check
        total = 0
        for order in range(self.MAX_ORDER + 1):
            for idx in self.free[order]:
                size = self.pages_of(order)
                c(self.P[idx].in_list, "free list member not marked")
                c(self.P[idx].property == size, "block size != order")
                c(idx % size == 0, "block not aligned to its size")
                total += self.P[idx].property
        return total


def run_buddy_tests(sim):
    c = sim.check
    c(sim.sum_free_pages() == sim.nr_free, "buddy: consistency before")

    # 1. 单页分配释放
    before = sim.nr_free
    p0, p1, p2 = sim.alloc_pages(1), sim.alloc_pages(1), sim.alloc_pages(1)
    c(None not in (p0, p1, p2), "buddy basic: alloc failed")
    c(len({p0, p1, p2}) == 3, "buddy basic: dup")
    c(all(sim.P[i].ref == 0 for i in (p0, p1, p2)), "buddy basic: ref")
    c(sim.nr_free == before - 3, "buddy basic: nr_free")
    c(sim.sum_free_pages() == sim.nr_free, "buddy basic: consistency")
    for i in (p0, p1, p2):
        sim.free_pages(i, 1)
    c(sim.nr_free == before, "buddy basic: restore")
    c(sim.sum_free_pages() == sim.nr_free, "buddy basic: consistency 2")

    # 2. 2 的幂粒度
    before = sim.nr_free
    p = sim.alloc_pages(3)
    c(p is not None, "buddy order: alloc(3) failed")
    c(p % 4 == 0, "buddy order: not 4-page aligned")
    c(sim.P[p].property == 4, "buddy order: property != 4")
    c(not sim.P[p].in_list, "buddy order: PageProperty should be cleared")
    c(sim.nr_free == before - 4, "buddy order: nr_free")
    sim.free_pages(p, 3)
    c(sim.nr_free == before, "buddy order: restore")

    # 3. 分裂与合并
    before = sim.nr_free
    order = 0
    while order <= sim.MAX_ORDER and not sim.free[order]:
        order += 1
    c(order <= sim.MAX_ORDER, "buddy split: no free block")
    size = sim.pages_of(order)
    p = sim.alloc_pages(size)
    c(p is not None and sim.P[p].property == size and p % size == 0, "buddy split: exact fit")
    sim.free_pages(p, size)
    c(sim.nr_free == before, "buddy split: restore after exact fit")

    # 3.2 逐级合并（与 C 版一致）：先把现成的 0 阶块借走，再分配 1 页迫使某个
    # 高阶块分裂，逐级核对伙伴块空闲，释放后各阶块数必须完全复原
    cnt_orig = [len(sim.free[o]) for o in range(sim.MAX_ORDER + 1)]
    pinned = []
    while len(pinned) < 8 and len(sim.free[0]) > 0:
        q = sim.alloc_pages(1)
        c(q is not None, "buddy merge: pin alloc failed")
        pinned.append(q)
    c(len(sim.free[0]) == 0, "buddy merge: order-0 blocks not drained")
    c(sim.nr_free == before - len(pinned), "buddy merge: nr_free after pinning")

    cnt_before = [len(sim.free[o]) for o in range(sim.MAX_ORDER + 1)]
    one = sim.alloc_pages(1)
    c(one is not None, "buddy merge: alloc 1 page failed")
    c(sim.nr_free == before - len(pinned) - 1, "buddy merge: nr_free -1")

    split = sim.MAX_ORDER + 1
    for o in range(sim.MAX_ORDER + 1):
        if len(sim.free[o]) + 1 == cnt_before[o]:
            c(split > sim.MAX_ORDER, "buddy merge: more than one order decreased")
            split = o
    c(1 <= split <= sim.MAX_ORDER, "buddy merge: alloc 1 page did not split")

    for k in range(split):
        size_k = 1 << k
        parent = one & ~((size_k << 1) - 1)
        sib = parent + (0 if (one & size_k) else size_k)
        c(sib < NPAGE - NBASE, "buddy merge: sibling out of managed range")
        c(sim.P[sib].property == size_k, "buddy merge: sibling property mismatch at order %d" % k)
        c(sim.P[sib].in_list and sib in sim.free[k], "buddy merge: sibling not in free list %d" % k)

    sim.free_pages(one, 1)
    c(sim.nr_free == before - len(pinned), "buddy merge: nr_free not restored after cascade")
    c([len(sim.free[o]) for o in range(sim.MAX_ORDER + 1)] == cnt_before,
      "buddy merge: per-order block counts not restored (cascade merge broken)")

    for q in pinned:
        sim.free_pages(q, 1)
    c(sim.nr_free == before, "buddy merge: nr_free not restored after unpinning")
    c([len(sim.free[o]) for o in range(sim.MAX_ORDER + 1)] == cnt_orig,
      "buddy merge: distribution after unpinning differs from entry state")

    # 4. 超量请求 + 单块上限分支（与 C 版一致：上限判定由 order_for_request 承担）
    before = sim.nr_free
    c(sim.alloc_pages(NPAGE - NBASE) is None, "buddy limit: total request should fail")
    c(sim.alloc_pages(sim.nr_free + 1) is None, "buddy limit: nr_free+1 should fail")

    max_block = 1 << sim.MAX_ORDER

    def order_for_request(n):
        order = sim.order_of(n)
        return order, (1 << order) < n

    c(order_for_request(1) == (0, False), "buddy limit: order of 1 page")
    c(order_for_request(3) == (2, False), "buddy limit: order of 3 pages")
    c(order_for_request(max_block) == (sim.MAX_ORDER, False), "buddy limit: 2^19 pages must be legal")
    c(order_for_request(max_block + 1) == (sim.MAX_ORDER, True), "buddy limit: 2^19+1 pages must be rejected")
    c(sim.alloc_pages(max_block + 1) is None, "buddy limit: over max block should fail")
    c(sim.nr_free == before, "buddy limit: nr_free changed")

    # 5. 批量耗尽
    before = sim.nr_free
    chunks = []
    while len(chunks) < 32:
        p = sim.alloc_pages(4096)
        if p is None:
            break
        chunks.append(p)
    c(len(chunks) > 0, "buddy exhaust: nothing allocated")
    c(sim.nr_free == before - len(chunks) * 4096, "buddy exhaust: nr_free")
    c(sim.sum_free_pages() == sim.nr_free, "buddy exhaust: consistency")
    for p in reversed(chunks):
        sim.free_pages(p, 4096)
    c(sim.nr_free == before, "buddy exhaust: restore")
    c(sim.sum_free_pages() == sim.nr_free, "buddy exhaust: consistency 2")


def main():
    total_checks = 0
    ok = 0
    # 内核结束地址不同 -> pages 数组末尾的位置不同，第一页空闲页的下标随之变化。
    # 0x330..0x380 覆盖内核镜像大小 1.3~1.4MB 的情形。
    for start in list(range(0x330, 0x381)):
        n = NPAGE - NBASE - start
        for name, cls, runner in (("best_fit", BestFit, run_best_fit_check),
                                  ("buddy", Buddy, run_buddy_tests)):
            sim = cls(start, n)
            try:
                runner(sim)
                # 每个用例结束后，空闲页总数必须与初始一致
                assert sim.nr_free_pages() == sim.sum_free_pages() if name == "buddy" else True
                ok += 1
                total_checks += sim.calls
            except AssertionError as exc:
                print("FAIL %s start=0x%x n=%d: %s" % (name, start, n, exc))
                raise
    print("all %d scenarios passed (%d assertions total)" % (ok, total_checks))

    # 额外：检查 buddy 初始化把空闲区拆分成了哪些块
    sim = Buddy(0x347, NPAGE - NBASE - 0x347)
    blocks = []
    for order in range(sim.MAX_ORDER + 1):
        for idx in sim.free[order]:
            blocks.append((idx, 1 << order))
    blocks.sort()
    print("buddy init blocks (idx, pages):", blocks)
    print("total free pages:", sim.nr_free)


if __name__ == "__main__":
    main()
