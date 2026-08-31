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

enum mmix_ipi_work_class {
  MMIX_IPI_WORK_TRANSLATION = 1ULL << 0,
};

typedef int (*ipi_work_handler)(uint64 classes, uint64 generation);

int ipi_validate(void);
int ipi_init(void);
int ipi_send(uint64 targets, uint64 *generation);
int ipi_send_work(uint64 targets, uint64 classes, uint64 work_generation,
                  uint64 *notification_generation);
int ipi_pending(int *pending);
int ipi_service(ipi_work_handler handler);
int ipi_acknowledged(uint32 target, uint64 generation, int *acknowledged);
int ipi_work_acknowledged(uint32 target, uint64 classes, uint64 generation,
                          int *acknowledged);
int ipi_progress(uint64 *received, uint64 *acknowledged);

#endif
