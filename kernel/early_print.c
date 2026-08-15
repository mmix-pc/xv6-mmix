#include "boot.h"
#include "early_uart.h"
#include "early_print.h"

static void
early_puts(const char *s)
{
  while (*s != 0)
    mmix_early_uart_putc(*s++);
}

static void
early_put_u64(uint64 value)
{
  char digits[20];
  int count = 0;

  do {
    digits[count++] = '0' + value % 10;
    value /= 10;
  } while (value != 0);

  while (count != 0)
    mmix_early_uart_putc(digits[--count]);
}

static void
early_put_int(int value)
{
  long wide = value;

  if (wide < 0) {
    mmix_early_uart_putc('-');
    wide = -wide;
  }

  early_put_u64((uint64)wide);
}

static void
early_put_hex64(uint64 value)
{
  early_puts("0x");
  for (int shift = 60; shift >= 0; shift -= 4) {
    uint64 digit = (value >> shift) & 0xf;
    mmix_early_uart_putc(digit < 10 ? '0' + digit : 'a' + digit - 10);
  }
}

static void
early_put_address(const char *name, uint64 address)
{
  early_puts(name);
  early_put_hex64(address);
}

void
mmix_early_print_boot(const struct mmix_boot_state *boot)
{
  const struct mmix_bootinfo *info = &boot->info;

  early_puts("xv6 MMIX boot\n");
  early_puts("bootinfo status: ");
  early_put_int(boot->bootinfo_status);
  early_puts("\nboot cpu id: ");
  early_put_u64(boot->startup_cpu_id);
  early_puts("\nbootinfo pa: ");
  early_put_hex64(boot->bootinfo_pa);
  mmix_early_uart_putc('\n');

  if (boot->bootinfo_status != MMIX_BOOTINFO_OK)
    return;

  early_puts("low ram: [");
  early_put_hex64(info->low_ram_base);
  early_puts(", ");
  early_put_hex64(info->low_ram_base + info->low_ram_size);
  early_puts(")\n");

  early_put_address("uart0: base=", info->uart_base);
  early_puts(" irq=");
  early_put_u64(info->uart_irq);
  mmix_early_uart_putc('\n');

  early_put_address("timer: base=", info->timer_base);
  early_puts(" irq-base=");
  early_put_u64(info->timer_irq_base);
  early_puts(" irq-count=");
  early_put_u64(info->timer_irq_count);
  mmix_early_uart_putc('\n');

  early_put_address("intc: base=", info->intc_base);
  early_puts(" irq-count=");
  early_put_u64(info->intc_irq_count);
  mmix_early_uart_putc('\n');

  early_put_address("virtio-mmio0: base=", info->virtio_mmio_base);
  early_puts(" irq=");
  early_put_u64(info->virtio_mmio_irq);
  early_puts(" count=");
  early_put_u64(info->virtio_mmio_count);
  mmix_early_uart_putc('\n');
}

void
mmix_early_print_paging(uint64 rv)
{
  early_puts("rV: ");
  early_put_hex64(rv);
  early_puts("\npaging enabled\n");
}

void
panic(char *message)
{
  early_puts("panic: ");
  early_puts(message);
  mmix_early_uart_putc('\n');

  for (;;)
    asm volatile("SWYM 0, 0, 0");
}
