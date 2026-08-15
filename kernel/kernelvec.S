#include "mmix.h"

.section .text
.p2align 4
.global mmix_kernel_trap_entry
.type mmix_kernel_trap_entry,@function
mmix_kernel_trap_entry:
        // Hardware supplies the interrupted rJ in $255. Preserve it and the
        // register-stack boundary before touching either kernel stack.
        PUT rW,r255
        GET r255,rG
        PUT rX,r255
        GET r255,rL
        PUT rY,r255
        GET r255,rO
        PUT rZ,r255
        GET r255,rS
        PUT rB,r255

        // A nested trap cannot safely reuse the partially populated frame.
        // GETA retains the negative physical alias of this entry point.
        GETA r255,mmix_trap_active
        LDOU r255,r255,0
        BNZ r255,.Lnested_trap
        GETA r255,mmix_trap_active
        STCO 1,r255,0

        // Pushing through $255 preserves every interrupted local register and
        // starts the handler with an empty local-register window.
        PUSHJ r255,.Ltrap_body

.Ltrap_resume:
        // The frame remains immediately below the restored software stack.
        NEGU r255,0,MMIX_TRAP_STATE_SIZE-MMIX_TRAP_RJ_OFFSET
        LDOU r255,r254,r255
        PUT rJ,r255
        GETA r255,mmix_trap_active
        STCO 0,r255,0
        NEGU r255,0,MMIX_TRAP_STATE_SIZE-MMIX_TRAP_RESTORE_RK_OFFSET
        LDOU r255,r254,r255
        RESUME 1

.Lnested_trap:
        PUT rK,0
.Lnested_wait:
        SWYM 0,0,0
        JMPB .Lnested_wait

.Ltrap_body:
        SETL r0,MMIX_TRAP_STATE_SIZE
        SUBU r254,r254,r0

        STOU r231,r254,MMIX_TRAP_GLOBAL_OFFSET(231)
        STOU r232,r254,MMIX_TRAP_GLOBAL_OFFSET(232)
        STOU r233,r254,MMIX_TRAP_GLOBAL_OFFSET(233)
        STOU r234,r254,MMIX_TRAP_GLOBAL_OFFSET(234)
        STOU r235,r254,MMIX_TRAP_GLOBAL_OFFSET(235)
        STOU r236,r254,MMIX_TRAP_GLOBAL_OFFSET(236)
        STOU r237,r254,MMIX_TRAP_GLOBAL_OFFSET(237)
        STOU r238,r254,MMIX_TRAP_GLOBAL_OFFSET(238)
        STOU r239,r254,MMIX_TRAP_GLOBAL_OFFSET(239)
        STOU r240,r254,MMIX_TRAP_GLOBAL_OFFSET(240)
        STOU r241,r254,MMIX_TRAP_GLOBAL_OFFSET(241)
        STOU r242,r254,MMIX_TRAP_GLOBAL_OFFSET(242)
        STOU r243,r254,MMIX_TRAP_GLOBAL_OFFSET(243)
        STOU r244,r254,MMIX_TRAP_GLOBAL_OFFSET(244)
        STOU r245,r254,MMIX_TRAP_GLOBAL_OFFSET(245)
        STOU r246,r254,MMIX_TRAP_GLOBAL_OFFSET(246)
        STOU r247,r254,MMIX_TRAP_GLOBAL_OFFSET(247)
        STOU r248,r254,MMIX_TRAP_GLOBAL_OFFSET(248)
        STOU r249,r254,MMIX_TRAP_GLOBAL_OFFSET(249)
        STOU r250,r254,MMIX_TRAP_GLOBAL_OFFSET(250)
        STOU r251,r254,MMIX_TRAP_GLOBAL_OFFSET(251)
        STOU r252,r254,MMIX_TRAP_GLOBAL_OFFSET(252)
        STOU r253,r254,MMIX_TRAP_GLOBAL_OFFSET(253)
        ADDU r1,r254,r0
        STOU r1,r254,MMIX_TRAP_GLOBAL_OFFSET(254)

        GET r1,rW
        STOU r1,r254,MMIX_TRAP_RJ_OFFSET
        GET r1,rBB
        STOU r1,r254,MMIX_TRAP_RBB_OFFSET
        GET r1,rWW
        STOU r1,r254,MMIX_TRAP_RWW_OFFSET
        GET r1,rXX
        STOU r1,r254,MMIX_TRAP_RXX_OFFSET
        GET r1,rYY
        STOU r1,r254,MMIX_TRAP_RYY_OFFSET
        GET r1,rZZ
        STOU r1,r254,MMIX_TRAP_RZZ_OFFSET
        GET r1,rQ
        STOU r1,r254,MMIX_TRAP_RQ_OFFSET
        GETA r1,mmix_trap_rk_shadow
        LDOU r1,r1,0
        STOU r1,r254,MMIX_TRAP_RESTORE_RK_OFFSET

        SETL r0,256
        ADDU r2,r254,r0
        GET r1,rX
        STOU r1,r2,MMIX_TRAP_RG_OFFSET-256
        GET r1,rY
        STOU r1,r2,MMIX_TRAP_RL_OFFSET-256
        GET r1,rZ
        STOU r1,r2,MMIX_TRAP_RO_OFFSET-256
        GET r1,rB
        STOU r1,r2,MMIX_TRAP_RS_OFFSET-256
        GET r1,rA
        STOU r1,r2,MMIX_TRAP_RA_OFFSET-256
        GET r1,rD
        STOU r1,r2,MMIX_TRAP_RD_OFFSET-256
        GET r1,rE
        STOU r1,r2,MMIX_TRAP_RE_OFFSET-256
        GET r1,rH
        STOU r1,r2,MMIX_TRAP_RH_OFFSET-256
        GET r1,rM
        STOU r1,r2,MMIX_TRAP_RM_OFFSET-256
        GET r1,rR
        STOU r1,r2,MMIX_TRAP_RR_OFFSET-256
        GET r1,rP
        STOU r1,r2,MMIX_TRAP_RP_OFFSET-256
        GET r1,rF
        STOU r1,r2,MMIX_TRAP_RF_OFFSET-256

        // Classification is limited to the frozen architectural encoding;
        // all handling policy belongs to the C dispatcher.
        SETL r231,MMIX_TRAP_CLASS_UNKNOWN
        LDOU r1,r254,MMIX_TRAP_RXX_OFFSET
        SETMH r2,0x00ff
        AND r3,r1,r2
        BNZ r3,.Lprogram_trap
        SETH r2,0x8000
        CMPU r3,r1,r2
        BZ r3,.Lexternal_candidate
        BN r1,.Lforced_candidate
        JMP .Lclass_ready

.Lprogram_trap:
        SETL r231,MMIX_TRAP_CLASS_PROGRAM
        JMP .Lclass_ready

.Lexternal_candidate:
        LDOU r1,r254,MMIX_TRAP_RQ_OFFSET
        LDOU r2,r254,MMIX_TRAP_RESTORE_RK_OFFSET
        AND r1,r1,r2
        SETL r2,MMIX_RQ_INTC
        AND r1,r1,r2
        BZ r1,.Lclass_ready
        SETL r231,MMIX_TRAP_CLASS_EXTERNAL
        JMP .Lclass_ready

.Lforced_candidate:
        SLU r1,r1,32
        BZ r1,.Lclass_ready
        SETL r231,MMIX_TRAP_CLASS_FORCED

.Lclass_ready:
        OR r232,r254,0
        GET r4,rJ
        GETA r0,mmix_kernel_trap
        SLU r0,r0,1
        SRU r0,r0,1
        PUSHGO r31,r0,0
        PUT rJ,r4

        SETL r0,256
        ADDU r2,r254,r0
        LDOU r1,r2,MMIX_TRAP_RA_OFFSET-256
        PUT rA,r1
        LDOU r1,r2,MMIX_TRAP_RD_OFFSET-256
        PUT rD,r1
        LDOU r1,r2,MMIX_TRAP_RE_OFFSET-256
        PUT rE,r1
        LDOU r1,r2,MMIX_TRAP_RH_OFFSET-256
        PUT rH,r1
        LDOU r1,r2,MMIX_TRAP_RM_OFFSET-256
        PUT rM,r1
        LDOU r1,r2,MMIX_TRAP_RR_OFFSET-256
        PUT rR,r1
        LDOU r1,r2,MMIX_TRAP_RP_OFFSET-256
        PUT rP,r1
        LDOU r1,r2,MMIX_TRAP_RF_OFFSET-256
        PUT rF,r1
        LDOU r1,r2,MMIX_TRAP_RG_OFFSET-256
        PUT rG,r1

        LDOU r1,r254,MMIX_TRAP_RBB_OFFSET
        PUT rBB,r1
        LDOU r1,r254,MMIX_TRAP_RWW_OFFSET
        PUT rWW,r1
        LDOU r1,r254,MMIX_TRAP_RXX_OFFSET
        PUT rXX,r1
        LDOU r1,r254,MMIX_TRAP_RYY_OFFSET
        PUT rYY,r1
        LDOU r1,r254,MMIX_TRAP_RZZ_OFFSET
        PUT rZZ,r1
        LDOU r1,r254,MMIX_TRAP_RQ_OFFSET
        PUT rQ,r1

        LDOU r231,r254,MMIX_TRAP_GLOBAL_OFFSET(231)
        LDOU r232,r254,MMIX_TRAP_GLOBAL_OFFSET(232)
        LDOU r233,r254,MMIX_TRAP_GLOBAL_OFFSET(233)
        LDOU r234,r254,MMIX_TRAP_GLOBAL_OFFSET(234)
        LDOU r235,r254,MMIX_TRAP_GLOBAL_OFFSET(235)
        LDOU r236,r254,MMIX_TRAP_GLOBAL_OFFSET(236)
        LDOU r237,r254,MMIX_TRAP_GLOBAL_OFFSET(237)
        LDOU r238,r254,MMIX_TRAP_GLOBAL_OFFSET(238)
        LDOU r239,r254,MMIX_TRAP_GLOBAL_OFFSET(239)
        LDOU r240,r254,MMIX_TRAP_GLOBAL_OFFSET(240)
        LDOU r241,r254,MMIX_TRAP_GLOBAL_OFFSET(241)
        LDOU r242,r254,MMIX_TRAP_GLOBAL_OFFSET(242)
        LDOU r243,r254,MMIX_TRAP_GLOBAL_OFFSET(243)
        LDOU r244,r254,MMIX_TRAP_GLOBAL_OFFSET(244)
        LDOU r245,r254,MMIX_TRAP_GLOBAL_OFFSET(245)
        LDOU r246,r254,MMIX_TRAP_GLOBAL_OFFSET(246)
        LDOU r247,r254,MMIX_TRAP_GLOBAL_OFFSET(247)
        LDOU r248,r254,MMIX_TRAP_GLOBAL_OFFSET(248)
        LDOU r249,r254,MMIX_TRAP_GLOBAL_OFFSET(249)
        LDOU r250,r254,MMIX_TRAP_GLOBAL_OFFSET(250)
        LDOU r251,r254,MMIX_TRAP_GLOBAL_OFFSET(251)
        LDOU r252,r254,MMIX_TRAP_GLOBAL_OFFSET(252)
        LDOU r253,r254,MMIX_TRAP_GLOBAL_OFFSET(253)
        LDOU r1,r254,MMIX_TRAP_GLOBAL_OFFSET(254)
        OR r254,r1,0
        POP 0,0
.size mmix_kernel_trap_entry,.-mmix_kernel_trap_entry
