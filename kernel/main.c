#include "types.h"
#include "mmix.h"
#include "defs.h"
#include "boot.h"
#include "diagnostic.h"
#include "platform.h"
#include "physmem.h"
#include "kalloc.h"
#include "kcontext.h"
#include "cpu.h"
#include "vm.h"

void main(void) __attribute__((noreturn));

static void
kernel_ready(void)
{
  struct kalloc_stats stats;

  if (boot_reclaim_stacks() != PHYSMEM_OK)
    panic("bootstrap release");
  kalloc_get_stats(&stats);
  if (stats.managed_pages != physmem_managed_pages() ||
      stats.reserved_pages != physmem_reserved_pages())
    panic("memory ownership");
  diagnostic_paging(mmix_rv_read());
  diagnostic_allocator(&stats);
  scheduler();
}

// start() jumps here after publishing the immutable platform description.
void
main(void)
{
  if (boot_wait_for_online() < 0)
    panic("CPU online");
  diagnostic_platform_checkpoint();

  // SMP remains at discovery until CPU-local interrupt setup is available.
  if (platform_cpu_count() != 1)
    for (;;)
      asm volatile("SWYM 0, 0, 0" ::: "memory");

  kinit();
  if (boot_reclaim_fdt() != PHYSMEM_OK)
    panic("FDT release");
  kvminit();
  kvminithart();
  kcontext_init();
  procinit();
  cpu_context_enter(kernel_ready);
}
