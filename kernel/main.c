#include "types.h"
#include "mmix.h"
#include "defs.h"
#include "boot.h"
#include "diagnostic.h"

void main(void) __attribute__((noreturn));

// start() jumps here after publishing the immutable platform description.
void
main(void)
{
  if (boot_wait_for_online() < 0)
    panic("CPU online");
  diagnostic_platform_checkpoint();

  // Keep this discovery boundary terminal until allocation consumes the
  // normalized FDT ownership ranges.
  for (;;)
    asm volatile("SWYM 0, 0, 0" ::: "memory");
}
