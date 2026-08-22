#ifndef XV6_MMIX_KCONTEXT_H
#define XV6_MMIX_KCONTEXT_H

#include "types.h"

struct context;

void kcontext_init(void);
void kcontext_prepare(struct context *, uint, void (*)(void));

#endif
