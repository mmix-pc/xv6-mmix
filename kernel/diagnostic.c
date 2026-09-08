#include "boot.h"
#include "kalloc.h"
#include "diagnostic.h"
#include "intc.h"
#include "platform.h"
#include "printk.h"
#include "vm.h"
#include "defs.h"

static void
diagnostic_putc(int c)
{
  uartputc_sync(c);
}

static void
diagnostic_puts(const char *s)
{
  while (*s != 0)
    diagnostic_putc(*s++);
}

static void
diagnostic_put_u64(uint64 value)
{
  char digits[20];
  int count = 0;

  do {
    digits[count++] = '0' + value % 10;
    value /= 10;
  } while (value != 0);

  while (count != 0)
    diagnostic_putc(digits[--count]);
}

static void
diagnostic_put_int(int value)
{
  long wide = value;

  if (wide < 0) {
    diagnostic_putc('-');
    wide = -wide;
  }

  diagnostic_put_u64((uint64)wide);
}

static void
diagnostic_put_hex64(uint64 value)
{
  diagnostic_puts("0x");
  for (int shift = 60; shift >= 0; shift -= 4) {
    uint64 digit = (value >> shift) & 0xf;
    diagnostic_putc(digit < 10 ? '0' + digit : 'a' + digit - 10);
  }
}

void
diagnostic_startup_failure(uint64 failure, int decode_status)
{
  diagnostic_puts("platform startup failure: code=");
  diagnostic_put_u64(failure);
  diagnostic_puts(" decode-status=");
  diagnostic_put_int(decode_status);
  diagnostic_putc('\n');
}

void
diagnostic_platform_checkpoint(void)
{
  uint64 cpu_count = platform_cpu_count();

  diagnostic_puts("xv6 MMIX platform checkpoint\n");
  diagnostic_puts("platform: generation=");
  diagnostic_put_u64(mmix_startup.generation);
  diagnostic_puts(" publications=");
  diagnostic_put_u64(mmix_startup.platform_publications);
  diagnostic_puts(" fdt=");
  diagnostic_put_hex64(platform_fdt_physical_address());
  diagnostic_puts(" cpus=");
  diagnostic_put_u64(cpu_count);
  diagnostic_putc('\n');
  diagnostic_puts("platform: arrived=");
  diagnostic_put_hex64(mmix_startup.arrived);
  diagnostic_puts(" online=");
  diagnostic_put_hex64(mmix_startup.online);
  diagnostic_putc('\n');
  for (uint64 cpu_id = 0; cpu_id < cpu_count; cpu_id++) {
    const struct mmix_boot_handoff *handoff = &mmix_boot_handoffs[cpu_id];

    diagnostic_puts("entry: cpu=");
    diagnostic_put_u64(cpu_id);
    diagnostic_puts(" fdt=");
    diagnostic_put_hex64(handoff->fdt_address);
    diagnostic_puts(" rl=");
    diagnostic_put_u64(handoff->entry_rl);
    diagnostic_puts(" ro=");
    diagnostic_put_hex64(handoff->entry_ro);
    diagnostic_puts(" rs=");
    diagnostic_put_hex64(handoff->entry_rs);
    // This is copied entry provenance, not the CPU's current stack or owner.
    diagnostic_puts(" stack=[");
    diagnostic_put_hex64(handoff->software_stack_base);
    diagnostic_puts(", ");
    diagnostic_put_hex64(handoff->software_stack_top);
    diagnostic_puts(") stage=");
    diagnostic_put_u64(mmix_startup.cpu_stage[cpu_id]);
    diagnostic_putc('\n');
  }
  diagnostic_puts("PLATFORM CHECKPOINT PASS cpus=");
  diagnostic_put_u64(cpu_count);
  diagnostic_puts(" generation=");
  diagnostic_put_u64(mmix_startup.generation);
  diagnostic_putc('\n');
}

void
diagnostic_allocator(const struct kalloc_stats *stats)
{
  diagnostic_puts("allocator: physical-pages=");
  diagnostic_put_u64(stats->physical_pages);
  diagnostic_puts(" managed-pages=");
  diagnostic_put_u64(stats->managed_pages);
  diagnostic_puts(" reserved-pages=");
  diagnostic_put_u64(stats->reserved_pages);
  diagnostic_puts(" free-pages=");
  diagnostic_put_u64(stats->free_pages);
  diagnostic_puts(" used-pages=");
  diagnostic_put_u64(stats->used_pages);
  diagnostic_puts(" reclaimed-pages=");
  diagnostic_put_u64(stats->reclaimed_pages);
  diagnostic_puts("\nallocator audit passed\n");
}

void
diagnostic_paging(uint64 rv)
{
  diagnostic_puts("rV: ");
  diagnostic_put_hex64(rv);
  diagnostic_puts("\nkernel page-table audit passed\npaging enabled\n");
}

void
diagnostic_startup(uint64 cpu_count, uint64 online)
{
  char mask[19];

  mask[0] = '0';
  mask[1] = 'x';
  for (int digit = 0; digit < 16; digit++) {
    int value = (online >> (60 - 4 * digit)) & 0xf;

    mask[2 + digit] = value < 10 ? '0' + value : 'a' + value - 10;
  }
  mask[18] = 0;
  for (uint64 cpu_id = 0; cpu_id < cpu_count; cpu_id++)
    printk("cpu %d: entered, online\n", (int)cpu_id);
  printk("startup online: %s\n", mask);
}

void
diagnostic_trap(const struct mmix_trap_diagnostic *diagnostic)
{
  diagnostic_puts(diagnostic->from_user ? "user trap: class=" :
                                         "kernel trap: class=");
  diagnostic_puts(diagnostic->event_class);
  diagnostic_puts(" cause=");
  diagnostic_puts(diagnostic->cause);
  diagnostic_puts(" pid=");
  diagnostic_put_int(diagnostic->pid);
  diagnostic_puts(" name=");
  diagnostic_puts(diagnostic->process_name);
  diagnostic_puts("\nrq=");
  diagnostic_put_hex64(diagnostic->rq);
  diagnostic_puts(" rk=");
  diagnostic_put_hex64(diagnostic->active_rk);
  diagnostic_puts(" restore-rk=");
  diagnostic_put_hex64(diagnostic->restore_rk);
  diagnostic_puts("\nrww=");
  diagnostic_put_hex64(diagnostic->rww);
  diagnostic_puts(" rxx=");
  diagnostic_put_hex64(diagnostic->rxx);
  diagnostic_puts("\nryy=");
  diagnostic_put_hex64(diagnostic->ryy);
  diagnostic_puts(" rzz=");
  diagnostic_put_hex64(diagnostic->rzz);
  diagnostic_puts("\nstate=");
  diagnostic_put_hex64(diagnostic->state);
  diagnostic_puts(" sp=");
  diagnostic_put_hex64(diagnostic->sp);
  diagnostic_puts(" fp=");
  diagnostic_put_hex64(diagnostic->fp);
  diagnostic_puts("\nro=");
  diagnostic_put_hex64(diagnostic->ro);
  diagnostic_puts(" rs=");
  diagnostic_put_hex64(diagnostic->rs);
  diagnostic_puts(" rl=");
  diagnostic_put_hex64(diagnostic->rl);
  // Stream indexed words instead of allocating bitmaps on the trap stack.
  for (uint32 word = 0; word < INTC_WORD_COUNT; word++) {
    uint64 pending;
    uint64 enabled;

    if (intc_pending(word, &pending) != MMIX_INTC_OK ||
        intc_enabled(word, &enabled) != MMIX_INTC_OK) {
      diagnostic_puts("\nintc-unavailable");
      break;
    }
    if (word != 0 && pending == 0 && enabled == 0)
      continue;
    diagnostic_puts("\nintc-word=");
    diagnostic_put_u64(word);
    diagnostic_puts(" pending=");
    diagnostic_put_hex64(pending);
    diagnostic_puts(" enabled=");
    diagnostic_put_hex64(enabled);
  }
  diagnostic_puts("\nclaim=");
  diagnostic_put_u64(diagnostic->intc_claim);
  diagnostic_puts(" timer-pending=");
  diagnostic_put_int(diagnostic->timer_pending);
  diagnostic_puts(" ipi-pending=");
  diagnostic_put_int(diagnostic->ipi_pending);
  diagnostic_puts(" ipi-received=");
  diagnostic_put_u64(diagnostic->ipi_received);
  diagnostic_puts(" ipi-acknowledged=");
  diagnostic_put_u64(diagnostic->ipi_acknowledged);
  diagnostic_putc('\n');
}
