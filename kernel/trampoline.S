#include "proc.h"
#include "cpu.h"
#include "memlayout.h"

#if MMIX_MAX_CPUS != 16
#error "update the user-trapframe CPU dispatch for the supported topology"
#endif

.macro MMIX_CPU_FIELD_ADDRESS result, scratch, offset
        GET \result,rU
        SRU \result,\result,MMIX_RU_CPU_ID_SHIFT
        MULU \result,\result,MMIX_CPU_SIZE
        GETA \scratch,cpus
        ADDU \result,\result,\scratch
        ADDU \result,\result,\offset
.endm

// Before SAVE only $255 is available: hardware preserved the interrupted
// user value in rBB. Select one fixed field address without touching a second
// user register, then load that CPU's published trapframe alias.
.macro MMIX_LOAD_USER_TRAPFRAME result
        GET \result,rU
        SRU \result,\result,MMIX_RU_CPU_ID_SHIFT
        BZ \result,.Luser_cpu_0_\@
        SUBU \result,\result,1
        BZ \result,.Luser_cpu_1_\@
        SUBU \result,\result,1
        BZ \result,.Luser_cpu_2_\@
        SUBU \result,\result,1
        BZ \result,.Luser_cpu_3_\@
        SUBU \result,\result,1
        BZ \result,.Luser_cpu_4_\@
        SUBU \result,\result,1
        BZ \result,.Luser_cpu_5_\@
        SUBU \result,\result,1
        BZ \result,.Luser_cpu_6_\@
        SUBU \result,\result,1
        BZ \result,.Luser_cpu_7_\@
        SUBU \result,\result,1
        BZ \result,.Luser_cpu_8_\@
        SUBU \result,\result,1
        BZ \result,.Luser_cpu_9_\@
        SUBU \result,\result,1
        BZ \result,.Luser_cpu_10_\@
        SUBU \result,\result,1
        BZ \result,.Luser_cpu_11_\@
        SUBU \result,\result,1
        BZ \result,.Luser_cpu_12_\@
        SUBU \result,\result,1
        BZ \result,.Luser_cpu_13_\@
        SUBU \result,\result,1
        BZ \result,.Luser_cpu_14_\@
        SUBU \result,\result,1
        BZ \result,.Luser_cpu_15_\@
        JMP .Luser_emergency
.Luser_cpu_0_\@:
        GETA \result,cpus+MMIX_CPU_USER_TRAPFRAME_OFFSET
        JMP .Luser_cpu_selected_\@
.Luser_cpu_1_\@:
        GETA \result,cpus+MMIX_CPU_SIZE+MMIX_CPU_USER_TRAPFRAME_OFFSET
        JMP .Luser_cpu_selected_\@
.Luser_cpu_2_\@:
        GETA \result,cpus+2*MMIX_CPU_SIZE+MMIX_CPU_USER_TRAPFRAME_OFFSET
        JMP .Luser_cpu_selected_\@
.Luser_cpu_3_\@:
        GETA \result,cpus+3*MMIX_CPU_SIZE+MMIX_CPU_USER_TRAPFRAME_OFFSET
        JMP .Luser_cpu_selected_\@
.Luser_cpu_4_\@:
        GETA \result,cpus+4*MMIX_CPU_SIZE+MMIX_CPU_USER_TRAPFRAME_OFFSET
        JMP .Luser_cpu_selected_\@
.Luser_cpu_5_\@:
        GETA \result,cpus+5*MMIX_CPU_SIZE+MMIX_CPU_USER_TRAPFRAME_OFFSET
        JMP .Luser_cpu_selected_\@
.Luser_cpu_6_\@:
        GETA \result,cpus+6*MMIX_CPU_SIZE+MMIX_CPU_USER_TRAPFRAME_OFFSET
        JMP .Luser_cpu_selected_\@
.Luser_cpu_7_\@:
        GETA \result,cpus+7*MMIX_CPU_SIZE+MMIX_CPU_USER_TRAPFRAME_OFFSET
        JMP .Luser_cpu_selected_\@
.Luser_cpu_8_\@:
        GETA \result,cpus+8*MMIX_CPU_SIZE+MMIX_CPU_USER_TRAPFRAME_OFFSET
        JMP .Luser_cpu_selected_\@
.Luser_cpu_9_\@:
        GETA \result,cpus+9*MMIX_CPU_SIZE+MMIX_CPU_USER_TRAPFRAME_OFFSET
        JMP .Luser_cpu_selected_\@
.Luser_cpu_10_\@:
        GETA \result,cpus+10*MMIX_CPU_SIZE+MMIX_CPU_USER_TRAPFRAME_OFFSET
        JMP .Luser_cpu_selected_\@
.Luser_cpu_11_\@:
        GETA \result,cpus+11*MMIX_CPU_SIZE+MMIX_CPU_USER_TRAPFRAME_OFFSET
        JMP .Luser_cpu_selected_\@
.Luser_cpu_12_\@:
        GETA \result,cpus+12*MMIX_CPU_SIZE+MMIX_CPU_USER_TRAPFRAME_OFFSET
        JMP .Luser_cpu_selected_\@
.Luser_cpu_13_\@:
        GETA \result,cpus+13*MMIX_CPU_SIZE+MMIX_CPU_USER_TRAPFRAME_OFFSET
        JMP .Luser_cpu_selected_\@
.Luser_cpu_14_\@:
        GETA \result,cpus+14*MMIX_CPU_SIZE+MMIX_CPU_USER_TRAPFRAME_OFFSET
        JMP .Luser_cpu_selected_\@
.Luser_cpu_15_\@:
        GETA \result,cpus+15*MMIX_CPU_SIZE+MMIX_CPU_USER_TRAPFRAME_OFFSET
.Luser_cpu_selected_\@:
        LDOU \result,\result,0
.endm

.section .text

// Entered through the negative rT/rTT alias with the user translation and
// both user stacks still active. This path saves the complete user
// continuation before installing the kernel translation or continuation.
.p2align 4
.global mmix_user_trap_entry
.type mmix_user_trap_entry,@function
mmix_user_trap_entry:
        MMIX_LOAD_USER_TRAPFRAME r255
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
        MMIX_LOAD_USER_TRAPFRAME r255
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
        MMIX_LOAD_USER_TRAPFRAME r255
        LDOU r255,r255,MMIX_PROC_TRAPFRAME_FLAGS_OFFSET
        CMPU r255,r255,MMIX_PROC_TRAPFRAME_ACTIVE
        BNZ r255,.Luser_emergency

        MMIX_LOAD_USER_TRAPFRAME r255
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
        MMIX_CPU_FIELD_ADDRESS r0,r1,MMIX_CPU_USER_TRAPFRAME_OFFSET
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
        MMIX_CPU_FIELD_ADDRESS r0,r1,MMIX_CPU_USER_TRAPFRAME_OFFSET
        LDOU r0,r0,0
        STCO 0,r0,MMIX_PROC_TRAPFRAME_KERNEL_STATE_OFFSET
        STCO MMIX_PROC_TRAPFRAME_READY,r0,MMIX_PROC_TRAPFRAME_FLAGS_OFFSET
        MMIX_CPU_FIELD_ADDRESS r0,r1,MMIX_CPU_USER_TRAPFRAME_OFFSET
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
        MMIX_CPU_FIELD_ADDRESS r0,r1,MMIX_CPU_USER_TRAPFRAME_OFFSET
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
        MMIX_CPU_FIELD_ADDRESS r0,r1,MMIX_CPU_USER_TRAPFRAME_OFFSET
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

        // Only $255 is scratch after UNSAVE. Keep the negative trapframe alias
        // temporarily in rBB, whose user value remains process-owned below.
        MMIX_LOAD_USER_TRAPFRAME r255
        PUT rBB,r255
        GET r255,rBB
        LDOU r255,r255,MMIX_PROC_TRAPFRAME_RWW_OFFSET
        PUT rWW,r255
        GET r255,rBB
        LDOU r255,r255,MMIX_PROC_TRAPFRAME_RXX_OFFSET
        PUT rXX,r255
        GET r255,rBB
        LDOU r255,r255,MMIX_PROC_TRAPFRAME_RYY_OFFSET
        PUT rYY,r255
        GET r255,rBB
        LDOU r255,r255,MMIX_PROC_TRAPFRAME_RZZ_OFFSET
        PUT rZZ,r255

        // C completed the entry-side handoff before it could schedule. Pair a
        // fresh GET with its current value; process snapshots are diagnostic.
        GET r255,rQ
        PUT rQ,r255

        GET r255,rBB
        LDOU r255,r255,MMIX_PROC_TRAPFRAME_USER_RF_OFFSET
        PUT rF,r255
        GET r255,rBB
        LDOU r255,r255,MMIX_PROC_TRAPFRAME_USER_RU_OFFSET
        PUT rU,r255
        GET r255,rBB
        LDOU r255,r255,MMIX_PROC_TRAPFRAME_USER_RI_OFFSET
        PUT rI,r255
        GET r255,rBB
        LDOU r255,r255,MMIX_PROC_TRAPFRAME_USER_RC_OFFSET
        PUT rC,r255

        GET r255,rBB
        LDOU r255,r255,MMIX_PROC_TRAPFRAME_RBB_OFFSET
        PUT rBB,r255
        MMIX_LOAD_USER_TRAPFRAME r255
        LDOU r255,r255,MMIX_PROC_TRAPFRAME_USER_RK_OFFSET
        RESUME 1

.Luser_emergency:
        PUT rK,0
.Luser_wait:
        SWYM 0,0,0
        JMPB .Luser_wait
.size mmix_user_resume,.-mmix_user_resume
