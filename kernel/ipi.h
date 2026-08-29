#ifndef XV6_MMIX_IPI_H
#define XV6_MMIX_IPI_H

#include "types.h"

enum mmix_ipi_status {
  MMIX_IPI_OK = 0,
  MMIX_IPI_BAD_ARGUMENT = -1,
  MMIX_IPI_BAD_PLATFORM = -2,
  MMIX_IPI_BAD_TARGET = -3,
  MMIX_IPI_BAD_STATE = -4,
};

int ipi_validate(void);
int ipi_init(void);
int ipi_send(uint64 targets, uint64 *generation);
int ipi_pending(int *pending);
int ipi_service(void);
int ipi_acknowledged(uint32 target, uint64 generation, int *acknowledged);
int ipi_progress(uint64 *received, uint64 *acknowledged);

#endif
