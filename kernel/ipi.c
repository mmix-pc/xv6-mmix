#include "boot.h"
#include "cpu.h"
#include "ipi.h"
#include "mmix.h"

enum {
  MMIX_IPI_REGISTER_SIZE = 8,
  MMIX_IPI_ACTIVE_OFFSET = 0x0000,
  MMIX_IPI_SEND_OFFSET = 0x0008,
  MMIX_IPI_CONTEXT_BASE = 0x0100,
  MMIX_IPI_CONTEXT_STRIDE = 0x20,
  MMIX_IPI_CONTEXT_STATUS_OFFSET = 0x00,
  MMIX_IPI_CONTEXT_CLEAR_OFFSET = 0x08,
  MMIX_IPI_STATUS_PENDING = 1 << 0,
};

struct ipi_target_state {
  uint64 requested;
  uint64 acknowledged;
  uint64 received;
  uint64 work_classes;
  uint64 work_generation;
  uint64 work_notification;
  uint64 work_acknowledged;
};

static struct ipi_target_state ipi_targets[MMIX_MAX_CPUS];
static uint64 ipi_generation;
static uint32 ipi_send_lock;

#define MMIX_IPI_WORK_VALID MMIX_IPI_WORK_TRANSLATION

static uint64
ipi_active_targets(void)
{
  return (1ULL << mmix_boot.info.ipi_target_count) - 1;
}

static volatile uint64 *
ipi_register(uint64 offset)
{
  return (volatile uint64 *)(mmix_boot.info.ipi_base + offset);
}

static uint64
ipi_context_register(uint32 target, uint64 offset)
{
  return MMIX_IPI_CONTEXT_BASE +
         (uint64)target * MMIX_IPI_CONTEXT_STRIDE + offset;
}

static uint64
ipi_read(uint64 offset)
{
  return *ipi_register(offset);
}

static void
ipi_write(uint64 offset, uint64 value)
{
  *ipi_register(offset) = value;
}

static int
ipi_platform_valid(void)
{
  const struct mmix_bootinfo *info = &mmix_boot.info;

  return mmix_boot.bootinfo_status == MMIX_BOOTINFO_OK &&
         (info->ipi_base & (MMIX_IPI_REGISTER_SIZE - 1)) == 0 &&
         info->boot_cpu_id == BOOT_CPU_ID && info->cpu_count > 0 &&
         info->cpu_count <= MMIX_MAX_CPUS &&
         info->ipi_target_count == info->cpu_count &&
         info->ipi_target_count <= IPI_TARGET_COUNT_MAX &&
         info->ipi_request_mask == MMIX_RQ_IPI &&
         ipi_read(MMIX_IPI_ACTIVE_OFFSET) == ipi_active_targets();
}

static int
ipi_current_valid(void)
{
  int id = cpuid();

  return ipi_platform_valid() && id >= 0 &&
         (uint64)id < mmix_boot.info.ipi_target_count;
}

static int
ipi_target_valid(uint32 target)
{
  return ipi_platform_valid() &&
         (uint64)target < mmix_boot.info.ipi_target_count;
}

static int
ipi_allocate_generation(uint64 *generation)
{
  uint64 current = __atomic_load_n(&ipi_generation, __ATOMIC_RELAXED);

  for (;;) {
    if (current == ~0ULL)
      return MMIX_IPI_BAD_STATE;
    if (__atomic_compare_exchange_n(&ipi_generation, &current, current + 1, 1,
                                    __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
      *generation = current + 1;
      return MMIX_IPI_OK;
    }
  }
}

int
ipi_validate(void)
{
  return ipi_platform_valid() ? MMIX_IPI_OK : MMIX_IPI_BAD_PLATFORM;
}

int
ipi_init(void)
{
  struct ipi_target_state *target;
  uint64 status;
  int id = cpuid();

  if (!ipi_current_valid())
    return MMIX_IPI_BAD_PLATFORM;

  target = &ipi_targets[id];
  __atomic_store_n(&target->requested, 0, __ATOMIC_RELAXED);
  __atomic_store_n(&target->acknowledged, 0, __ATOMIC_RELAXED);
  __atomic_store_n(&target->received, 0, __ATOMIC_RELAXED);
  __atomic_store_n(&target->work_classes, 0, __ATOMIC_RELAXED);
  __atomic_store_n(&target->work_generation, 0, __ATOMIC_RELAXED);
  __atomic_store_n(&target->work_notification, 0, __ATOMIC_RELAXED);
  __atomic_store_n(&target->work_acknowledged, 0, __ATOMIC_RELAXED);
  status = ipi_context_register(id, MMIX_IPI_CONTEXT_STATUS_OFFSET);
  if ((ipi_read(status) & ~MMIX_IPI_STATUS_PENDING) != 0)
    return MMIX_IPI_BAD_STATE;
  ipi_write(ipi_context_register(id, MMIX_IPI_CONTEXT_CLEAR_OFFSET),
            MMIX_IPI_STATUS_PENDING);
  if (ipi_read(status) != 0)
    return MMIX_IPI_BAD_STATE;
  return MMIX_IPI_OK;
}

int
ipi_send(uint64 targets, uint64 *generation)
{
  uint64 allocated;
  uint64 active;
  int status;

  if (generation == 0 || targets == 0)
    return MMIX_IPI_BAD_ARGUMENT;
  if (!ipi_current_valid())
    return MMIX_IPI_BAD_PLATFORM;
  active = ipi_active_targets();
  if ((targets & ~active) != 0)
    return MMIX_IPI_BAD_TARGET;

  // Serialize generation publication so a later sender cannot overtake an
  // earlier sender between allocating its generation and notifying targets.
  while (__atomic_exchange_n(&ipi_send_lock, 1, __ATOMIC_ACQUIRE) != 0)
    ;
  status = ipi_allocate_generation(&allocated);
  if (status != MMIX_IPI_OK) {
    __atomic_store_n(&ipi_send_lock, 0, __ATOMIC_RELEASE);
    return status;
  }

  for (uint32 target = 0; target < mmix_boot.info.ipi_target_count; target++) {
    if ((targets & (1ULL << target)) == 0)
      continue;
    __atomic_store_n(&ipi_targets[target].requested, allocated,
                     __ATOMIC_RELEASE);
  }

  ipi_write(MMIX_IPI_SEND_OFFSET, targets);
  *generation = allocated;
  __atomic_store_n(&ipi_send_lock, 0, __ATOMIC_RELEASE);
  return MMIX_IPI_OK;
}

int
ipi_send_work(uint64 targets, uint64 classes, uint64 work_generation,
              uint64 *notification_generation)
{
  struct ipi_target_state *target;
  uint64 acknowledged;
  uint64 allocated;
  uint64 active;
  uint64 published;
  int status;

  if (notification_generation == 0 || work_generation == 0 || targets == 0 ||
      classes == 0 ||
      (classes & ~MMIX_IPI_WORK_VALID) != 0)
    return MMIX_IPI_BAD_ARGUMENT;
  if (!ipi_current_valid())
    return MMIX_IPI_BAD_PLATFORM;
  active = ipi_active_targets();
  if ((targets & ~active) != 0)
    return MMIX_IPI_BAD_TARGET;

  while (__atomic_exchange_n(&ipi_send_lock, 1, __ATOMIC_ACQUIRE) != 0)
    ;
  for (uint32 id = 0; id < mmix_boot.info.ipi_target_count; id++) {
    if ((targets & (1ULL << id)) == 0)
      continue;
    target = &ipi_targets[id];
    published = __atomic_load_n(&target->work_generation, __ATOMIC_ACQUIRE);
    acknowledged =
      __atomic_load_n(&target->work_acknowledged, __ATOMIC_ACQUIRE);
    if (__atomic_load_n(&target->work_classes, __ATOMIC_ACQUIRE) != 0 ||
        published != acknowledged) {
      __atomic_store_n(&ipi_send_lock, 0, __ATOMIC_RELEASE);
      return MMIX_IPI_BAD_STATE;
    }
  }
  status = ipi_allocate_generation(&allocated);
  if (status != MMIX_IPI_OK) {
    __atomic_store_n(&ipi_send_lock, 0, __ATOMIC_RELEASE);
    return status;
  }

  for (uint32 id = 0; id < mmix_boot.info.ipi_target_count; id++) {
    if ((targets & (1ULL << id)) == 0)
      continue;
    target = &ipi_targets[id];
    __atomic_store_n(&target->work_generation, work_generation,
                     __ATOMIC_RELAXED);
    __atomic_store_n(&target->work_notification, allocated,
                     __ATOMIC_RELAXED);
    __atomic_store_n(&target->work_classes, classes, __ATOMIC_RELEASE);
    __atomic_store_n(&target->requested, allocated, __ATOMIC_RELEASE);
  }

  ipi_write(MMIX_IPI_SEND_OFFSET, targets);
  *notification_generation = allocated;
  __atomic_store_n(&ipi_send_lock, 0, __ATOMIC_RELEASE);
  return MMIX_IPI_OK;
}

int
ipi_pending(int *pending)
{
  uint64 status;

  if (pending == 0)
    return MMIX_IPI_BAD_ARGUMENT;
  if (!ipi_current_valid())
    return MMIX_IPI_BAD_PLATFORM;
  status = ipi_read(ipi_context_register(cpuid(),
                                         MMIX_IPI_CONTEXT_STATUS_OFFSET));
  if ((status & ~MMIX_IPI_STATUS_PENDING) != 0)
    return MMIX_IPI_BAD_STATE;
  *pending = (status & MMIX_IPI_STATUS_PENDING) != 0;
  return MMIX_IPI_OK;
}

int
ipi_service(ipi_work_handler handler)
{
  struct ipi_target_state *target;
  uint64 acknowledged;
  uint64 latest;
  uint64 observed;
  uint64 received;
  uint64 work_acknowledged;
  uint64 work_classes;
  uint64 work_generation;
  uint64 work_notification;
  int pending;
  int id = cpuid();

  if (ipi_pending(&pending) != MMIX_IPI_OK || !pending)
    return MMIX_IPI_BAD_STATE;
  target = &ipi_targets[id];
  observed = __atomic_load_n(&target->requested, __ATOMIC_ACQUIRE);
  acknowledged = __atomic_load_n(&target->acknowledged, __ATOMIC_RELAXED);
  received = __atomic_load_n(&target->received, __ATOMIC_RELAXED);
  if (observed == 0 || observed <= acknowledged || received == ~0ULL)
    return MMIX_IPI_BAD_STATE;

  work_classes = __atomic_load_n(&target->work_classes, __ATOMIC_ACQUIRE);
  if ((work_classes & ~MMIX_IPI_WORK_VALID) != 0)
    return MMIX_IPI_BAD_STATE;
  if (work_classes != 0) {
    work_generation =
      __atomic_load_n(&target->work_generation, __ATOMIC_RELAXED);
    work_notification =
      __atomic_load_n(&target->work_notification, __ATOMIC_RELAXED);
    work_acknowledged =
      __atomic_load_n(&target->work_acknowledged, __ATOMIC_RELAXED);
    if (work_generation == 0 || work_notification == 0 ||
        work_generation <= work_acknowledged)
      return MMIX_IPI_BAD_STATE;
    if (work_notification <= observed) {
      if (handler == 0 ||
          handler(work_classes, work_generation) != MMIX_IPI_OK)
        return MMIX_IPI_BAD_STATE;
      // Clear the published work before acknowledging it. A sender that sees
      // the acknowledgement can then safely reuse this target's work slot.
      __atomic_store_n(&target->work_classes, 0, __ATOMIC_RELAXED);
      __atomic_store_n(&target->work_acknowledged, work_generation,
                       __ATOMIC_RELEASE);
    }
  }

  ipi_write(ipi_context_register(id, MMIX_IPI_CONTEXT_CLEAR_OFFSET),
            MMIX_IPI_STATUS_PENDING);
  __atomic_store_n(&target->acknowledged, observed, __ATOMIC_RELEASE);
  __atomic_store_n(&target->received, received + 1, __ATOMIC_RELEASE);

  // A send racing the clear must leave a hardware notification for the newer
  // guest-memory generation, even if its first device write was coalesced.
  latest = __atomic_load_n(&target->requested, __ATOMIC_ACQUIRE);
  if (latest != observed)
    ipi_write(MMIX_IPI_SEND_OFFSET, 1ULL << id);
  return MMIX_IPI_OK;
}

int
ipi_work_acknowledged(uint32 target, uint64 classes, uint64 generation,
                      int *acknowledged)
{
  struct ipi_target_state *state;
  uint64 published;
  uint64 value;

  if (acknowledged == 0 || generation == 0 || classes == 0 ||
      (classes & ~MMIX_IPI_WORK_VALID) != 0)
    return MMIX_IPI_BAD_ARGUMENT;
  if (!ipi_current_valid())
    return MMIX_IPI_BAD_PLATFORM;
  if (!ipi_target_valid(target))
    return MMIX_IPI_BAD_TARGET;
  state = &ipi_targets[target];
  published = __atomic_load_n(&state->work_generation, __ATOMIC_ACQUIRE);
  value = __atomic_load_n(&state->work_acknowledged, __ATOMIC_ACQUIRE);
  if (published < generation || value > published)
    return MMIX_IPI_BAD_STATE;
  *acknowledged = value >= generation;
  return MMIX_IPI_OK;
}

int
ipi_acknowledged(uint32 target, uint64 generation, int *acknowledged)
{
  uint64 value;

  if (acknowledged == 0 || generation == 0)
    return MMIX_IPI_BAD_ARGUMENT;
  if (!ipi_current_valid())
    return MMIX_IPI_BAD_PLATFORM;
  if (!ipi_target_valid(target))
    return MMIX_IPI_BAD_TARGET;
  value = __atomic_load_n(&ipi_targets[target].acknowledged,
                          __ATOMIC_ACQUIRE);
  *acknowledged = value >= generation;
  return MMIX_IPI_OK;
}

int
ipi_progress(uint64 *received, uint64 *acknowledged)
{
  struct ipi_target_state *target;

  if (received == 0 || acknowledged == 0)
    return MMIX_IPI_BAD_ARGUMENT;
  if (!ipi_current_valid())
    return MMIX_IPI_BAD_PLATFORM;
  target = &ipi_targets[cpuid()];
  *received = __atomic_load_n(&target->received, __ATOMIC_ACQUIRE);
  *acknowledged = __atomic_load_n(&target->acknowledged, __ATOMIC_ACQUIRE);
  return MMIX_IPI_OK;
}

_Static_assert((MMIX_IPI_ACTIVE_OFFSET & (MMIX_IPI_REGISTER_SIZE - 1)) == 0 &&
                 (MMIX_IPI_SEND_OFFSET & (MMIX_IPI_REGISTER_SIZE - 1)) == 0 &&
                 (MMIX_IPI_CONTEXT_BASE &
                  (MMIX_IPI_REGISTER_SIZE - 1)) == 0 &&
                 (MMIX_IPI_CONTEXT_STRIDE &
                  (MMIX_IPI_REGISTER_SIZE - 1)) == 0,
               "MMIX IPI registers must be octa-aligned");
