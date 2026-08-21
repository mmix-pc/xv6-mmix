#include "proc.h"

.section .text

// Entered through the negative rT/rTT alias with the user translation and
// both user stacks still active. This path saves the complete user
// continuation before installing the kernel translation or continuation.
.p2align 4
.global mmix_user_trap_entry
.type mmix_user_trap_entry,@function
mmix_user_trap_entry:
        GETA r255,mmix_user_trapframe
        LDOU r255,r255,0
        BZ r255,.Luser_emergency
        LDOU r255,r255,MMIX_PROC_TRAPFRAME_FLAGS_OFFSET
        CMPU r255,r255,MMIX_PROC_TRAPFRAME_ACTIVE
        BNZ r255,.Luser_emergency

        GETA r255,mmix_user_trapframe
        LDOU r255,r255,0
        STCO MMIX_PROC_TRAPFRAME_ENTERING,r255,MMIX_PROC_TRAPFRAME_FLAGS_OFFSET

        // SAVE must precede C, scheduling, kernel UNSAVE, and the rV switch.
        // Hardware already copied the interrupted user $255 to rBB.
        SAVE r255
        GETA r0,mmix_user_trapframe
        LDOU r0,r0,0
        STOU r255,r0,MMIX_PROC_TRAPFRAME_USER_STATE_OFFSET

        GET r1,rV
        LDOU r2,r0,MMIX_PROC_TRAPFRAME_USER_RV_OFFSET
        CMPU r1,r1,r2
        BNZ r1,.Luser_emergency
        LDOU r1,r0,MMIX_PROC_TRAPFRAME_KERNEL_STATE_OFFSET
        BZ r1,.Luser_emergency
        LDOU r1,r0,MMIX_PROC_TRAPFRAME_RESERVED_OFFSET
        BNZ r1,.Luser_emergency

        GET r1,rC
        STOU r1,r0,MMIX_PROC_TRAPFRAME_USER_RC_OFFSET
        GET r1,rI
        STOU r1,r0,MMIX_PROC_TRAPFRAME_USER_RI_OFFSET
        GET r1,rU
        STOU r1,r0,MMIX_PROC_TRAPFRAME_USER_RU_OFFSET
        GET r1,rF
        STOU r1,r0,MMIX_PROC_TRAPFRAME_USER_RF_OFFSET
        GET r1,rBB
        STOU r1,r0,MMIX_PROC_TRAPFRAME_RBB_OFFSET
        GET r1,rWW
        STOU r1,r0,MMIX_PROC_TRAPFRAME_RWW_OFFSET
        GET r1,rXX
        STOU r1,r0,MMIX_PROC_TRAPFRAME_RXX_OFFSET
        GET r1,rYY
        STOU r1,r0,MMIX_PROC_TRAPFRAME_RYY_OFFSET
        GET r1,rZZ
        STOU r1,r0,MMIX_PROC_TRAPFRAME_RZZ_OFFSET
        GET r1,rQ
        STOU r1,r0,MMIX_PROC_TRAPFRAME_RQ_OFFSET

        SETH r1,((MMIX_KERNEL_RV >> 48) & 0xffff)
        ORMH r1,((MMIX_KERNEL_RV >> 32) & 0xffff)
        ORML r1,((MMIX_KERNEL_RV >> 16) & 0xffff)
        ORL r1,(MMIX_KERNEL_RV & 0xffff)
        SYNC 3
        SYNC 6
        PUT rV,r1
        LDOU r255,r0,MMIX_PROC_TRAPFRAME_KERNEL_STATE_OFFSET
        UNSAVE r255

        // The kernel continuation is live again. Retire the destructively
        // consumed state and unpublish user ownership before returning to C.
        GETA r0,mmix_user_trapframe
        LDOU r0,r0,0
        STCO 0,r0,MMIX_PROC_TRAPFRAME_KERNEL_STATE_OFFSET
        STCO MMIX_PROC_TRAPFRAME_READY,r0,MMIX_PROC_TRAPFRAME_FLAGS_OFFSET
        GETA r0,mmix_user_trapframe
        STCO 0,r0,0
        POP 0,0
.size mmix_user_trap_entry,.-mmix_user_trap_entry

// Save the current kernel continuation, enter the negative restore stub, and
// return here only after the next user trap has saved the user continuation.
.p2align 4
.global mmix_user_resume
.type mmix_user_resume,@function
mmix_user_resume:
        SAVE r255
        GETA r0,mmix_user_trapframe
        LDOU r0,r0,0
        BZ r0,.Luser_emergency
        STOU r255,r0,MMIX_PROC_TRAPFRAME_KERNEL_STATE_OFFSET

        GET r1,rV
        SETH r2,((MMIX_KERNEL_RV >> 48) & 0xffff)
        ORMH r2,((MMIX_KERNEL_RV >> 32) & 0xffff)
        ORML r2,((MMIX_KERNEL_RV >> 16) & 0xffff)
        ORL r2,(MMIX_KERNEL_RV & 0xffff)
        CMPU r1,r1,r2
        BNZ r1,.Luser_emergency

        GETA r1,.Luser_restore
        SETH r2,0x8000
        OR r1,r1,r2
        GO r0,r1,0

.Luser_restore:
        GETA r0,mmix_user_trapframe
        LDOU r0,r0,0
        BZ r0,.Luser_emergency
        LDOU r1,r0,MMIX_PROC_TRAPFRAME_FLAGS_OFFSET
        CMPU r1,r1,MMIX_PROC_TRAPFRAME_ACTIVE
        BNZ r1,.Luser_emergency
        LDOU r1,r0,MMIX_PROC_TRAPFRAME_RESERVED_OFFSET
        BNZ r1,.Luser_emergency

        LDOU r255,r0,MMIX_PROC_TRAPFRAME_USER_STATE_OFFSET
        LDOU r1,r0,MMIX_PROC_TRAPFRAME_USER_RV_OFFSET
        SYNC 3
        SYNC 6
        PUT rV,r1
        UNSAVE r255

        // Only $255 is scratch after UNSAVE; RESUME restores the interrupted
        // user value from rBB. Reload the negative trapframe alias each time.
        GETA r255,mmix_user_trapframe
        LDOU r255,r255,0
        LDOU r255,r255,MMIX_PROC_TRAPFRAME_RBB_OFFSET
        PUT rBB,r255
        GETA r255,mmix_user_trapframe
        LDOU r255,r255,0
        LDOU r255,r255,MMIX_PROC_TRAPFRAME_RWW_OFFSET
        PUT rWW,r255
        GETA r255,mmix_user_trapframe
        LDOU r255,r255,0
        LDOU r255,r255,MMIX_PROC_TRAPFRAME_RXX_OFFSET
        PUT rXX,r255
        GETA r255,mmix_user_trapframe
        LDOU r255,r255,0
        LDOU r255,r255,MMIX_PROC_TRAPFRAME_RYY_OFFSET
        PUT rYY,r255
        GETA r255,mmix_user_trapframe
        LDOU r255,r255,0
        LDOU r255,r255,MMIX_PROC_TRAPFRAME_RZZ_OFFSET
        PUT rZZ,r255

        // Complete the architectural GET/PUT request handoff using the value
        // selected by C rather than restoring a stale hardware snapshot.
        GET r255,rQ
        GETA r255,mmix_user_trapframe
        LDOU r255,r255,0
        LDOU r255,r255,MMIX_PROC_TRAPFRAME_RQ_OFFSET
        PUT rQ,r255

        GETA r255,mmix_user_trapframe
        LDOU r255,r255,0
        LDOU r255,r255,MMIX_PROC_TRAPFRAME_USER_RF_OFFSET
        PUT rF,r255
        GETA r255,mmix_user_trapframe
        LDOU r255,r255,0
        LDOU r255,r255,MMIX_PROC_TRAPFRAME_USER_RU_OFFSET
        PUT rU,r255
        GETA r255,mmix_user_trapframe
        LDOU r255,r255,0
        LDOU r255,r255,MMIX_PROC_TRAPFRAME_USER_RI_OFFSET
        PUT rI,r255
        GETA r255,mmix_user_trapframe
        LDOU r255,r255,0
        LDOU r255,r255,MMIX_PROC_TRAPFRAME_USER_RC_OFFSET
        PUT rC,r255

        GETA r255,mmix_user_trapframe
        LDOU r255,r255,0
        LDOU r255,r255,MMIX_PROC_TRAPFRAME_USER_RK_OFFSET
        RESUME 1

.Luser_emergency:
        PUT rK,0
.Luser_wait:
        SWYM 0,0,0
        JMPB .Luser_wait
.size mmix_user_resume,.-mmix_user_resume
