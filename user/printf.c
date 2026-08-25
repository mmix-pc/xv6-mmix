#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

#include <stdarg.h>

static char digits[] = "0123456789ABCDEF";

static void
putc(int fd, char c)
{
  write(fd, &c, 1);
}

// Use the format specifier's native C type so va_arg matches the caller;
// a fixed-width typedef such as uint64 may have a different underlying type.
static void
printunsigned(int fd, unsigned long long x, int base)
{
  char buf[20];
  int i;

  i = 0;
  do {
    buf[i++] = digits[x % base];
  } while ((x /= base) != 0);

  while (--i >= 0)
    putc(fd, buf[i]);
}

static void
printsigned(int fd, long long value, int base)
{
  unsigned long long magnitude = value;

  if (value < 0) {
    putc(fd, '-');
    magnitude = 0 - magnitude;
  }
  printunsigned(fd, magnitude, base);
}

static void
printptr(int fd, uint64 x)
{
  int i;
  putc(fd, '0');
  putc(fd, 'x');
  for (i = 0; i < (sizeof(uint64) * 2); i++, x <<= 4)
    putc(fd, digits[x >> (sizeof(uint64) * 8 - 4)]);
}

// Print to the given fd. Only understands %d, %x, %p, %c, %s.
void
vprintf(int fd, const char *fmt, va_list ap)
{
  char *s;
  int c0, c1, c2, i, state;

  state = 0;
  for (i = 0; fmt[i]; i++) {
    c0 = fmt[i] & 0xff;
    if (state == 0) {
      if (c0 == '%') {
        state = '%';
      } else {
        putc(fd, c0);
      }
    } else if (state == '%') {
      c1 = c2 = 0;
      if (c0)
        c1 = fmt[i + 1] & 0xff;
      if (c1)
        c2 = fmt[i + 2] & 0xff;
      if (c0 == 'd') {
        printsigned(fd, va_arg(ap, int), 10);
      } else if (c0 == 'l' && c1 == 'd') {
        printsigned(fd, va_arg(ap, long), 10);
        i += 1;
      } else if (c0 == 'l' && c1 == 'l' && c2 == 'd') {
        printsigned(fd, va_arg(ap, long long), 10);
        i += 2;
      } else if (c0 == 'u') {
        printunsigned(fd, va_arg(ap, unsigned int), 10);
      } else if (c0 == 'l' && c1 == 'u') {
        printunsigned(fd, va_arg(ap, unsigned long), 10);
        i += 1;
      } else if (c0 == 'l' && c1 == 'l' && c2 == 'u') {
        printunsigned(fd, va_arg(ap, unsigned long long), 10);
        i += 2;
      } else if (c0 == 'x') {
        printunsigned(fd, va_arg(ap, unsigned int), 16);
      } else if (c0 == 'l' && c1 == 'x') {
        printunsigned(fd, va_arg(ap, unsigned long), 16);
        i += 1;
      } else if (c0 == 'l' && c1 == 'l' && c2 == 'x') {
        printunsigned(fd, va_arg(ap, unsigned long long), 16);
        i += 2;
      } else if (c0 == 'p') {
        printptr(fd, (uint64)va_arg(ap, void *));
      } else if (c0 == 'c') {
        putc(fd, va_arg(ap, int));
      } else if (c0 == 's') {
        if ((s = va_arg(ap, char *)) == 0)
          s = "(null)";
        for (; *s; s++)
          putc(fd, *s);
      } else if (c0 == '%') {
        putc(fd, '%');
      } else {
        // Unknown % sequence.  Print it to draw attention.
        putc(fd, '%');
        putc(fd, c0);
      }

      state = 0;
    }
  }
}

void
fprintf(int fd, const char *fmt, ...)
{
  va_list ap;

  va_start(ap, fmt);
  vprintf(fd, fmt, ap);
  va_end(ap);
}

void
printf(const char *fmt, ...)
{
  va_list ap;

  va_start(ap, fmt);
  vprintf(1, fmt, ap);
  va_end(ap);
}
