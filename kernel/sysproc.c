#include "types.h"
#include "mmix.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "proc.h"
#include "vm.h"

uint64
sys_exit(void)
{
  int n;
  argint(0, &n);
  kexit(n);
  return 0; // not reached
}

uint64
sys_getpid(void)
{
  return myproc()->pid;
}

uint64
sys_fork(void)
{
  return kfork();
}

uint64
sys_wait(void)
{
  uint64 p;
  argaddr(0, &p);
  return kwait(p);
}

uint64
sys_sbrk(void)
{
  uint64 addr;
  int t;
  int n;

  argint(0, &n);
  argint(1, &t);
  addr = myproc()->sz;

  if (t != SBRK_EAGER && t != SBRK_LAZY)
    return -1;
  if (n == 0)
    return addr;
  if (t == SBRK_EAGER || n < 0) {
    if (growproc(n) < 0)
      return -1;
  } else {
    struct proc *p = myproc();

    // Forced translation supplies the missing address to vmfault(); ordinary
    // processes retain hardware walks and avoid needless translation traps.
    if (p == 0)
      return -1;
    acquire(&p->lock);
    if (p->state != RUNNING || p->vm_owner_cpu != cpuid() ||
        p->pagetable == 0 || p->sz < MMIX_USER_IMAGE_BASE ||
        (uint64)n > MMIX_USER_HEAP_LIMIT - p->sz) {
      release(&p->lock);
      return -1;
    }
    if (p->lazy_start == 0)
      p->lazy_start = p->sz;
    p->sz += (uint64)n;
    p->pagetable->rv =
      mmix_user_rv_set_function(p->pagetable->rv, MMIX_RV_F_SOFTWARE);
    p->trapframe->user_rv = p->pagetable->rv;
    proc_vm_mutated(p);
    release(&p->lock);
  }
  return addr;
}

uint64
sys_pause(void)
{
  int n;
  uint ticks0;

  argint(0, &n);
  if (n < 0)
    n = 0;
  acquire(&tickslock);
  ticks0 = ticks;
  while (ticks - ticks0 < n) {
    if (killed(myproc())) {
      release(&tickslock);
      return -1;
    }
    sleep(&ticks, &tickslock);
  }
  release(&tickslock);
  return 0;
}

uint64
sys_kill(void)
{
  int pid;

  argint(0, &pid);
  return kkill(pid);
}

// return how many clock tick interrupts have occurred
// since start.
uint64
sys_uptime(void)
{
  uint xticks;

  acquire(&tickslock);
  xticks = ticks;
  release(&tickslock);
  return xticks;
}
