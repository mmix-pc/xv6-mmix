#ifndef XV6_VM_H
#define XV6_VM_H

#include "mmix.h"

#define SBRK_EAGER 1
#define SBRK_LAZY  2

extern pagetable_t kernel_pagetable;

void kvminit(void);
void kvminithart(void);
pte_t *walk(pagetable_t pagetable, uint64 va, int alloc);
uint64 walkaddr(pagetable_t pagetable, uint64 va);
int mappages(pagetable_t pagetable, uint64 va, uint64 size, uint64 pa,
             int permissions);
void uvmunmap(pagetable_t pagetable, uint64 va, uint64 npages, int do_free);
void freewalk(pagetable_t pagetable);

#endif
