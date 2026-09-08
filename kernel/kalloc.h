#ifndef XV6_MMIX_KALLOC_H
#define XV6_MMIX_KALLOC_H

struct physmem_release;

struct kalloc_stats {
  uint64 physical_pages;
  uint64 managed_pages;
  uint64 free_pages;
};

void *kalloc(void);
void *kalloc_contiguous(uint count);
void *kalloc_dma(void);
void kalloc_publish_release(const struct physmem_release *);
void kfree(void *pa);
void kinit(void);

void kalloc_get_stats(struct kalloc_stats *stats);
uint64 kalloc_free_pages(void);
int kalloc_page_is_managed(void *pa);
int kalloc_page_is_dma(void *pa);

#endif
