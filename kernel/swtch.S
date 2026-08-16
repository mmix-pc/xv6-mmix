#include "mmix.h"

.section .text
.global swtch
.type swtch,@function
swtch:
        // r231 and r232 are global ABI argument registers, so SAVE records
        // their values without making them inaccessible to this transition.
        SAVE r255
        STOU r255,r231,MMIX_CONTEXT_STATE_OFFSET
        LDOU r255,r232,MMIX_CONTEXT_STATE_OFFSET
        UNSAVE r255

        // Established contexts return from their suspended call. For a new
        // context, this consumes its synthetic outer hole and enters rJ.
        POP 0,0
.size swtch,.-swtch
