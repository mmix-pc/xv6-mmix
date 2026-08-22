#include "memlayout.h"

.section .text
.global _entry
.type _entry,@function
_entry:
        // QEMU enters with the boot CPU ID and boot-info physical address in
        // local registers $0 and $1. Preserve them in the Clang ABI argument
        // registers before changing the global-register boundary.
        OR r231, r0, 0
        OR r232, r1, 0

        // Clang reserves $231-$255 as global registers. The software stack is
        // separate from QEMU's register-stack backing, whose rO/rS state must
        // remain unchanged.
        PUT rG, 231
        SETML r254, ((BOOT_STACK_TOP >> 16) & 0xffff)
        PUT rL, 0

        // PUSHJ uses $31 as the call hole and passes the preserved QEMU values
        // to the C boot boundary in $231 and $232.
        PUSHJ r31, start

.Lwait:
        JMP .Lwait
.size _entry, .-_entry
