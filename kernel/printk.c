//
// formatted console output -- printk, panic.
//

#include <stdarg.h>

#include "types.h"
#include "param.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "fs.h"
#include "file.h"
#include "memlayout.h"
#include "mmix.h"
#include "proc.h"
#include "defs.h"
#include "printk.h"

volatile int panicking; // printing a panic message
volatile int panicked;  // spinning forever at end of a panic

// lock to avoid interleaving concurrent printk's.
static struct {
  struct spinlock lock;
} pr;

static char digits[] = "0123456789abcdef";

// Use the format specifier's native C type so va_arg matches the caller;
// a fixed-width typedef such as uint64 may have a different underlying type.
static void
printunsigned(unsigned long long x, int base)
{
  char buf[20];
  int i;

  i = 0;
  do {
    buf[i++] = digits[x % base];
  } while ((x /= base) != 0);

  while (--i >= 0)
    consputc(buf[i]);
}

static void
printsigned(long long value, int base)
{
  unsigned long long magnitude = value;

  if (value < 0) {
    consputc('-');
    magnitude = 0 - magnitude;
  }
  printunsigned(magnitude, base);
}

static void
printptr(uint64 x)
{
  int i;
  consputc('0');
  consputc('x');
  for (i = 0; i < (sizeof(uint64) * 2); i++, x <<= 4)
    consputc(digits[x >> (sizeof(uint64) * 8 - 4)]);
}

// Print to the console.
int
printk(char *fmt, ...)
{
  va_list ap;
  int i, cx, c0, c1, c2;
  char *s;

  if (panicking == 0)
    acquire(&pr.lock);

  va_start(ap, fmt);
  for (i = 0; (cx = fmt[i] & 0xff) != 0; i++) {
    if (cx != '%') {
      consputc(cx);
      continue;
    }
    i++;
    c0 = fmt[i + 0] & 0xff;
    c1 = c2 = 0;
    if (c0)
      c1 = fmt[i + 1] & 0xff;
    if (c1)
      c2 = fmt[i + 2] & 0xff;
    if (c0 == 'd') {
      printsigned(va_arg(ap, int), 10);
    } else if (c0 == 'l' && c1 == 'd') {
      printsigned(va_arg(ap, long), 10);
      i += 1;
    } else if (c0 == 'l' && c1 == 'l' && c2 == 'd') {
      printsigned(va_arg(ap, long long), 10);
      i += 2;
    } else if (c0 == 'u') {
      printunsigned(va_arg(ap, unsigned int), 10);
    } else if (c0 == 'l' && c1 == 'u') {
      printunsigned(va_arg(ap, unsigned long), 10);
      i += 1;
    } else if (c0 == 'l' && c1 == 'l' && c2 == 'u') {
      printunsigned(va_arg(ap, unsigned long long), 10);
      i += 2;
    } else if (c0 == 'x') {
      printunsigned(va_arg(ap, unsigned int), 16);
    } else if (c0 == 'l' && c1 == 'x') {
      printunsigned(va_arg(ap, unsigned long), 16);
      i += 1;
    } else if (c0 == 'l' && c1 == 'l' && c2 == 'x') {
      printunsigned(va_arg(ap, unsigned long long), 16);
      i += 2;
    } else if (c0 == 'p') {
      printptr((uint64)va_arg(ap, void *));
    } else if (c0 == 'c') {
      consputc(va_arg(ap, int));
    } else if (c0 == 's') {
      if ((s = va_arg(ap, char *)) == 0)
        s = "(null)";
      for (; *s; s++)
        consputc(*s);
    } else if (c0 == '%') {
      consputc('%');
    } else if (c0 == 0) {
      break;
    } else {
      // Print unknown % sequence to draw attention.
      consputc('%');
      consputc(c0);
    }
  }
  va_end(ap);

  if (panicking == 0)
    release(&pr.lock);

  return 0;
}

void
panic(char *s)
{
  mmix_intr_mask_write(0);
  panicking = 1;
  printk("panic: ");
  printk("%s\n", s);
  panicked = 1; // freeze uart output from other CPUs
  for (;;)
    ;
}

void
printkinit(void)
{
  initlock(&pr.lock, "pr");
}
