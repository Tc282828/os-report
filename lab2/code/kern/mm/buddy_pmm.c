#include <buddy_pmm.h>
#include <list.h>
#include <pmm.h>

/* 2^20 pages are 4 GiB, larger than the memory mapped by this lab kernel. */
#define BUDDY_MAX_ORDER 20

static list_entry_t buddy_free_lists[BUDDY_MAX_ORDER + 1];
static size_t buddy_nr_free;
static struct Page *buddy_base;
static size_t buddy_npages;
static unsigned int buddy_max_order;

static size_t
buddy_order_size(unsigned int order) {
    return (size_t)1 << order;
}

static unsigned int
buddy_floor_order(size_t pages_count) {
    unsigned int order = 0;
    while (order < BUDDY_MAX_ORDER &&
           buddy_order_size(order + 1) <= pages_count) {
        order++;
    }
    return order;
}

static unsigned int
buddy_ceil_order(size_t pages_count) {
    unsigned int order = 0;
    size_t block_pages = 1;
    while (block_pages < pages_count && order < BUDDY_MAX_ORDER) {
        order++;
        block_pages <<= 1;
    }
    return block_pages >= pages_count ? order : BUDDY_MAX_ORDER + 1;
}

static void
buddy_add_block(struct Page *base, unsigned int order) {
    base->property = buddy_order_size(order);
    SetPageProperty(base);
    list_add_before(&buddy_free_lists[order], &base->page_link);
}

static void
buddy_init(void) {
    for (unsigned int order = 0; order <= BUDDY_MAX_ORDER; order++) {
        list_init(&buddy_free_lists[order]);
    }
    buddy_nr_free = 0;
    buddy_base = NULL;
    buddy_npages = 0;
    buddy_max_order = 0;
}

static void
buddy_init_memmap(struct Page *base, size_t n) {
    assert(n > 0);
    assert(buddy_base == NULL);

    buddy_base = base;
    buddy_npages = n;
    buddy_max_order = buddy_floor_order(n);

    for (struct Page *page = base; page != base + n; page++) {
        assert(PageReserved(page));
        page->flags = 0;
        page->property = 0;
        set_page_ref(page, 0);
    }

    size_t offset = 0;
    while (offset < n) {
        size_t remaining = n - offset;
        unsigned int order = buddy_floor_order(remaining);
        if (order > buddy_max_order) {
            order = buddy_max_order;
        }

        size_t block_pages = buddy_order_size(order);
        while (order > 0 && (offset & (block_pages - 1)) != 0) {
            order--;
            block_pages >>= 1;
        }

        buddy_add_block(base + offset, order);
        offset += block_pages;
    }
    buddy_nr_free = n;
}

static struct Page *
buddy_alloc_pages(size_t n) {
    assert(n > 0);
    if (n > buddy_nr_free) {
        return NULL;
    }

    unsigned int wanted_order = buddy_ceil_order(n);
    if (wanted_order > buddy_max_order) {
        return NULL;
    }

    unsigned int found_order = wanted_order;
    while (found_order <= buddy_max_order &&
           list_empty(&buddy_free_lists[found_order])) {
        found_order++;
    }
    if (found_order > buddy_max_order) {
        return NULL;
    }

    list_entry_t *entry = list_next(&buddy_free_lists[found_order]);
    struct Page *block = le2page(entry, page_link);
    list_del(entry);
    ClearPageProperty(block);
    block->property = 0;

    while (found_order > wanted_order) {
        found_order--;
        struct Page *right_half = block + buddy_order_size(found_order);
        buddy_add_block(right_half, found_order);
    }

    size_t allocated_pages = buddy_order_size(wanted_order);
    block->property = allocated_pages;
    buddy_nr_free -= allocated_pages;
    return block;
}

static void
buddy_free_pages(struct Page *base, size_t n) {
    assert(n > 0);
    assert(buddy_base != NULL && base >= buddy_base);

    unsigned int order = buddy_ceil_order(n);
    assert(order <= buddy_max_order);
    size_t block_pages = buddy_order_size(order);
    size_t offset = base - buddy_base;

    assert(offset + block_pages <= buddy_npages);
    assert((offset & (block_pages - 1)) == 0);
    assert(!PageProperty(base) && base->property == block_pages);

    for (struct Page *page = base; page != base + block_pages; page++) {
        assert(!PageReserved(page) && !PageProperty(page));
        page->flags = 0;
        page->property = 0;
        set_page_ref(page, 0);
    }
    buddy_nr_free += block_pages;

    while (order < buddy_max_order) {
        size_t buddy_offset = offset ^ block_pages;
        if (buddy_offset + block_pages > buddy_npages) {
            break;
        }

        struct Page *other = buddy_base + buddy_offset;
        if (!PageProperty(other) || other->property != block_pages) {
            break;
        }

        list_del(&other->page_link);
        ClearPageProperty(other);
        other->property = 0;
        if (buddy_offset < offset) {
            base = other;
            offset = buddy_offset;
        }
        order++;
        block_pages <<= 1;
    }

    buddy_add_block(base, order);
}

static size_t
buddy_nr_free_pages(void) {
    return buddy_nr_free;
}

static void
buddy_check(void) {
    size_t free_before = buddy_nr_free_pages();
    struct Page *one = buddy_alloc_pages(1);
    struct Page *three = buddy_alloc_pages(3);
    struct Page *five = buddy_alloc_pages(5);

    assert(one != NULL && three != NULL && five != NULL);
    assert(one != three && one != five && three != five);
    assert(!PageProperty(one) && !PageProperty(three) && !PageProperty(five));
    assert(buddy_nr_free_pages() == free_before - 1 - 4 - 8);

    buddy_free_pages(three, 3);
    buddy_free_pages(one, 1);
    buddy_free_pages(five, 5);
    assert(buddy_nr_free_pages() == free_before);
}

const struct pmm_manager buddy_pmm_manager = {
    .name = "buddy_pmm_manager",
    .init = buddy_init,
    .init_memmap = buddy_init_memmap,
    .alloc_pages = buddy_alloc_pages,
    .free_pages = buddy_free_pages,
    .nr_free_pages = buddy_nr_free_pages,
    .check = buddy_check,
};
