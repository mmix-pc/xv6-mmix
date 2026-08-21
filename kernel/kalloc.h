#ifndef XV6_MMIX_KALLOC_H
#define XV6_MMIX_KALLOC_H

void *kalloc(void);
void *kalloc_contiguous(uint count);
void kfree(void *pa);
void kinit(void);

uint64 kalloc_free_pages(void);
int kalloc_page_is_managed(void *pa);

#endif
