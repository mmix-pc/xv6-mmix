#ifndef XV6_MMIX_KCONTEXT_H
#define XV6_MMIX_KCONTEXT_H

#include "types.h"

struct context;

void kcontext_init(void);
void kcontext_prepare(struct context *, uint, void (*)(void));
void kcontext_prepare_arg(struct context *, uint, void (*)(uint64), uint64);
int kcontext_current_valid(const struct context *, uint);

#endif
