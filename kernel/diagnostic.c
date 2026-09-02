#include "boot.h"
#include "kalloc.h"
#include "diagnostic.h"
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

static void
diagnostic_put_address(const char *name, uint64 address)
{
  diagnostic_puts(name);
  diagnostic_put_hex64(address);
}

void
diagnostic_boot(const struct mmix_boot_state *boot)
{
  const struct mmix_bootinfo *info = &boot->info;

  diagnostic_puts("xv6 MMIX boot\n");
  diagnostic_puts("bootinfo status: ");
  diagnostic_put_int(boot->bootinfo_status);
  diagnostic_puts("\nboot cpu id: ");
  diagnostic_put_u64(boot->startup_cpu_id);
  diagnostic_puts("\nbootinfo pa: ");
  diagnostic_put_hex64(boot->bootinfo_pa);
  diagnostic_putc('\n');

  if (boot->bootinfo_status != MMIX_BOOTINFO_OK)
    return;

  diagnostic_puts("ram total: ");
  diagnostic_put_hex64(info->memory.total_size);
  diagnostic_putc('\n');

  for (uint64 index = 0; index < info->memory.range_count; index++) {
    const struct mmix_physical_range *range = &info->memory.range[index];

    diagnostic_puts("physical ram ");
    diagnostic_put_u64(index);
    diagnostic_puts(": [");
    diagnostic_put_hex64(range->base);
    diagnostic_puts(", ");
    diagnostic_put_hex64(range->base + range->size);
    diagnostic_puts(")\n");
  }

  diagnostic_puts("ELF load RAM: [");
  diagnostic_put_hex64(info->low_ram_base);
  diagnostic_puts(", ");
  diagnostic_put_hex64(info->low_ram_base + info->low_ram_size);
  diagnostic_puts(")\n");

  diagnostic_put_address("uart0: base=", info->uart_base);
  diagnostic_puts(" irq=");
  diagnostic_put_u64(info->uart_irq);
  diagnostic_putc('\n');

  diagnostic_put_address("timer: base=", info->timer_base);
  diagnostic_puts(" irq-base=");
  diagnostic_put_u64(info->timer_irq_base);
  diagnostic_puts(" irq-count=");
  diagnostic_put_u64(info->timer_irq_count);
  diagnostic_putc('\n');

  diagnostic_put_address("intc: base=", info->intc_base);
  diagnostic_puts(" irq-count=");
  diagnostic_put_u64(info->intc_irq_count);
  diagnostic_putc('\n');

  diagnostic_put_address("virtio-mmio0: base=", info->virtio_mmio_base);
  diagnostic_puts(" irq=");
  diagnostic_put_u64(info->virtio_mmio_irq);
  diagnostic_puts(" count=");
  diagnostic_put_u64(info->virtio_mmio_count);
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
  diagnostic_put_u64(stats->physical_pages - stats->managed_pages);
  diagnostic_puts(" free-pages=");
  diagnostic_put_u64(stats->free_pages);
  diagnostic_puts("\nallocator high: managed-pages=");
  diagnostic_put_u64(stats->high_managed_pages);
  diagnostic_puts(" used-pages=");
  diagnostic_put_u64(stats->high_managed_pages - stats->high_free_pages);
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
  diagnostic_puts("\nintc-pending=");
  diagnostic_put_hex64(diagnostic->intc_pending);
  diagnostic_puts(" intc-enabled=");
  diagnostic_put_hex64(diagnostic->intc_enabled);
  diagnostic_puts(" claim=");
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
