#ifndef XV6_MMIX_KCONTEXT_H
#define XV6_MMIX_KCONTEXT_H

#include "types.h"

struct context;

void mmix_kcontext_init(void);
void mmix_kcontext_prepare(struct context *, uint, void (*)(void));

#endif
