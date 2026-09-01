#ifndef XV6_VM_H
#define XV6_VM_H

#include "mmix.h"

#define SBRK_EAGER 1
#define SBRK_LAZY  2

extern pagetable_t kernel_pagetable;

// Pages detached from a live page table but retained until shootdown completes.
struct vm_reclaim {
  void *pages;
  uint64 count;
};

void kvminit(void);
void kvminithart(void);
pagetable_t uvmcreate(uint asn);
uint64 uvmalloc(pagetable_t pagetable, uint64 oldsz, uint64 newsz,
                int permissions);
uint64 uvmdealloc(pagetable_t pagetable, uint64 oldsz, uint64 newsz);
uint64 uvmdealloc_deferred(pagetable_t pagetable, uint64 oldsz, uint64 newsz,
                           struct vm_reclaim *reclaim);
void uvmreclaim(struct vm_reclaim *reclaim);
int uvmretire_local(pagetable_t pagetable, uint64 *result);
uint64 vmfault(pagetable_t pagetable, uint64 va, int permissions);
int uvmallocstacks(pagetable_t pagetable);
int uvmcopy(pagetable_t old, pagetable_t new, uint64 sz);
void uvmfree(pagetable_t pagetable, uint64 sz);
void uvmclear(pagetable_t pagetable, uint64 va);
pte_t *walk(pagetable_t pagetable, uint64 va, int alloc);
uint64 walkaddr(pagetable_t pagetable, uint64 va);
int mappages(pagetable_t pagetable, uint64 va, uint64 size, uint64 pa,
             int permissions);
void uvmunmap(pagetable_t pagetable, uint64 va, uint64 npages, int do_free);
void freewalk(pagetable_t pagetable);
int copyout(pagetable_t pagetable, uint64 dstva, char *src, uint64 len);
int copyin(pagetable_t pagetable, char *dst, uint64 srcva, uint64 len);
int copyinstr(pagetable_t pagetable, char *dst, uint64 srcva, uint64 max);

#endif
