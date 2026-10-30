#ifndef __KERN_MM_SLUB_PMM_H__
#define __KERN_MM_SLUB_PMM_H__

#include <defs.h>

void slub_init(void);
void *slub_alloc(size_t size);
void slub_free(void *ptr);

size_t slub_nr_slabs(void);
size_t slub_nr_total_objects(void);
size_t slub_nr_inuse_objects(void);

void slub_check(void);

#endif /* !__KERN_MM_SLUB_PMM_H__ */
