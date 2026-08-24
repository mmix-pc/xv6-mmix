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
        BZ r255,.Ltranslation_try
        CMPU r255,r255,MMIX_PROC_TRAPFRAME_ENTERING
        BNZ r255,.Luser_emergency

.Ltranslation_try:
        // SAVE and UNSAVE may themselves miss while another forced
        // translation is pending. Resolve an existing leaf without touching
        // the register stack; only a genuinely absent leaf enters C.
        GETA r255,mmix_user_trapframe
        LDOU r255,r255,0
        STOU r253,r255,MMIX_PROC_TRAPFRAME_TRANSLATION_R253_OFFSET
        STOU r254,r255,MMIX_PROC_TRAPFRAME_TRANSLATION_R254_OFFSET
        OR r253,r255,0

        GET r254,rXX
        SRU r254,r254,32
        SETML r255,0x0300
        CMPU r254,r254,r255
        BNZ r254,.Ltranslation_miss

        GET r255,rYY
        SRU r255,r255,61
        BZ r255,.Ltranslation_segment0
        CMPU r255,r255,3
        BZ r255,.Ltranslation_segment3
        JMP .Ltranslation_miss

.Ltranslation_segment3:
        LDOU r255,r253,MMIX_PROC_TRAPFRAME_USER_RV_OFFSET
        SETMH r254,0x00ff
        ORML r254,0xffff
        ORL r254,0xe000
        AND r255,r255,r254
        SETH r254,0x8000
        OR r255,r255,r254
        INCL r255,0x4000
        GET r254,rYY
        SRU r254,r254,10
        SLU r254,r254,51
        SRU r254,r254,51
        LDOU r255,r255,r254
        JMP .Ltranslation_leaf

.Ltranslation_segment0:
        LDOU r255,r253,MMIX_PROC_TRAPFRAME_USER_RV_OFFSET
        SETMH r254,0x00ff
        ORML r254,0xffff
        ORL r254,0xe000
        AND r255,r255,r254
        SETH r254,0x8000
        OR r255,r255,r254
        GET r254,rYY
        SRU r254,r254,20
        SLU r254,r254,51
        SRU r254,r254,51
        BZ r254,.Ltranslation_segment0_leaf
        INCL r255,0x2000
        LDOU r255,r255,r254
        BZ r255,.Ltranslation_miss
        SETMH r254,0xffff
        ORML r254,0xffff
        ORL r254,0xe000
        AND r255,r255,r254
        SETH r254,0x8000
        OR r255,r255,r254

.Ltranslation_segment0_leaf:
        GET r254,rYY
        SRU r254,r254,10
        SLU r254,r254,51
        SRU r254,r254,51
        LDOU r255,r255,r254

.Ltranslation_leaf:
        BZ r255,.Ltranslation_miss
        PUT rZZ,r255
        LDOU r254,r253,MMIX_PROC_TRAPFRAME_TRANSLATION_R254_OFFSET
        LDOU r253,r253,MMIX_PROC_TRAPFRAME_TRANSLATION_R253_OFFSET
        RESUME 1

.Ltranslation_miss:
        LDOU r254,r253,MMIX_PROC_TRAPFRAME_TRANSLATION_R254_OFFSET
        LDOU r253,r253,MMIX_PROC_TRAPFRAME_TRANSLATION_R253_OFFSET
        GETA r255,mmix_user_trapframe
        LDOU r255,r255,0
        LDOU r255,r255,MMIX_PROC_TRAPFRAME_FLAGS_OFFSET
        CMPU r255,r255,MMIX_PROC_TRAPFRAME_ACTIVE
        BNZ r255,.Luser_emergency

        GETA r255,mmix_user_trapframe
        LDOU r255,r255,0
        STOU r254,r255,MMIX_PROC_TRAPFRAME_TRANSLATION_R254_OFFSET
        OR r254,r255,0
        GET r255,rBB
        STOU r255,r254,MMIX_PROC_TRAPFRAME_RBB_OFFSET
        GET r255,rWW
        STOU r255,r254,MMIX_PROC_TRAPFRAME_RWW_OFFSET
        GET r255,rXX
        STOU r255,r254,MMIX_PROC_TRAPFRAME_RXX_OFFSET
        GET r255,rYY
        STOU r255,r254,MMIX_PROC_TRAPFRAME_RYY_OFFSET
        GET r255,rZZ
        STOU r255,r254,MMIX_PROC_TRAPFRAME_RZZ_OFFSET
        GET r255,rQ
        STOU r255,r254,MMIX_PROC_TRAPFRAME_RQ_OFFSET
        STCO MMIX_PROC_TRAPFRAME_ENTERING,r254,MMIX_PROC_TRAPFRAME_FLAGS_OFFSET
        LDOU r254,r254,MMIX_PROC_TRAPFRAME_TRANSLATION_R254_OFFSET

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

        // C completed the entry-side handoff before it could schedule. Pair a
        // fresh GET with its current value; process snapshots are diagnostic.
        GET r255,rQ
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
