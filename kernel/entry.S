#include "memlayout.h"
#include "mmix.h"

.section .text
.global _entry
.type _entry,@function
_entry:
        // The Linux direct-boot ABI supplies the CPU ID and FDT address in
        // $0 and $1. Preserve all entry state before changing rG or rL.
        OR r231, r0, 0
        OR r232, r1, 0
        GET r233, rL
        GET r234, rO
        GET r235, rS

        // Contain an invalid ID before it can select writable memory. CMPU
        // returns a negative value only for IDs in [0, MMIX_MAX_CPUS).
        CMPU r236,r231,MMIX_MAX_CPUS
        BN r236,.Lcpu_valid
.Linvalid_cpu:
        SWYM 0,0,0
        JMPB .Linvalid_cpu

        // Clang reserves $231-$255 as global registers. The software stack is
        // separate from QEMU's register-stack backing, whose rO/rS state must
        // remain unchanged.
.Lcpu_valid:
        PUT rG, 231
        // rU is CPU-local, privileged to write, and excluded from SAVE. Its
        // stable usage-pattern byte therefore carries the kernel CPU ID.
        SLU r236,r231,MMIX_RU_CPU_ID_SHIFT
        PUT rU,r236
        GETA r254,%geta(kernel_boot_stacks_end)
        SLU r236,r231,MMIX_PAGE_SHIFT
        SUBU r254,r254,r236
        PUT rL, 0

        // PUSHJ uses $31 as the call hole. The five preserved Linux ABI values
        // occupy the Clang argument registers $231 through $235.
        PUSHJ r31, start

.Lwait:
        JMP .Lwait
.size _entry, .-_entry
