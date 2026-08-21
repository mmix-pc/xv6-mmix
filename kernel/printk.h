#ifndef XV6_MMIX_PRINTK_H
#define XV6_MMIX_PRINTK_H

int printk(char *, ...) __attribute__((format(printf, 1, 2)));
void panic(char *) __attribute__((noreturn));
void printkinit(void);
extern volatile int panicking;
extern volatile int panicked;

#endif
