// Thin named wrappers around the compiler's __atomic_* builtins.
//
// The wrappers keep memory orders and the 32-bit/64-bit width at the call
// site implicit while remaining zero-cost: they expand to exactly the same
// __atomic_* expression that was previously written inline, so the MMIX
// backend still lowers them to CSWAP/LDV/STCO + SYNC sequences.
#ifndef MMIX_ATOMIC_H
#define MMIX_ATOMIC_H

#define atomic_load_acquire(p) \
  __atomic_load_n((p), __ATOMIC_ACQUIRE)

#define atomic_load_relaxed(p) \
  __atomic_load_n((p), __ATOMIC_RELAXED)

#define atomic_store_release(p, value) \
  __atomic_store_n((p), (value), __ATOMIC_RELEASE)

#define atomic_store_relaxed(p, value) \
  __atomic_store_n((p), (value), __ATOMIC_RELAXED)

#define atomic_exchange_acquire(p, value) \
  __atomic_exchange_n((p), (value), __ATOMIC_ACQUIRE)

#define atomic_fetch_or_release(p, mask) \
  __atomic_fetch_or((p), (mask), __ATOMIC_RELEASE)

#define atomic_fetch_or_relaxed(p, mask) \
  __atomic_fetch_or((p), (mask), __ATOMIC_RELAXED)

// Strong CAS, RELAXED success and failure.
#define atomic_cas_relaxed(p, expected, desired) \
  __atomic_compare_exchange_n((p), (expected), (desired), 0, \
                             __ATOMIC_RELAXED, __ATOMIC_RELAXED)

// Weak CAS, RELAXED success and failure.
#define atomic_cas_relaxed_weak(p, expected, desired) \
  __atomic_compare_exchange_n((p), (expected), (desired), 1, \
                             __ATOMIC_RELAXED, __ATOMIC_RELAXED)

// Strong CAS, ACQUIRE success and RELAXED failure.
#define atomic_cas_acquire(p, expected, desired) \
  __atomic_compare_exchange_n((p), (expected), (desired), 0, \
                             __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)

// Strong CAS, ACQ_REL success and ACQUIRE failure.
#define atomic_cas_acq_rel(p, expected, desired) \
  __atomic_compare_exchange_n((p), (expected), (desired), 0, \
                             __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)

// Strong CAS, RELEASE success and ACQUIRE failure.
#define atomic_cas_release_acquire(p, expected, desired) \
  __atomic_compare_exchange_n((p), (expected), (desired), 0, \
                             __ATOMIC_RELEASE, __ATOMIC_ACQUIRE)

#endif
