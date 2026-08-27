#ifndef XV6_MMIX_KALLOC_H
#define XV6_MMIX_KALLOC_H

void *kalloc(void);
void *kalloc_contiguous(uint count);
void *kalloc_dma(void);
void kfree(void *pa);
void kinit(void);
void kinit_reclaimed(void);

uint64 kalloc_free_pages(void);
int kalloc_page_is_managed(void *pa);
int kalloc_page_is_dma(void *pa);

#endif
