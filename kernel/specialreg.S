.section .text

// Clang requires explicit floating-environment modeling for PUT rA and permits
// it only in module-level assembly.
.global mmix_ra_write
.type mmix_ra_write,@function
mmix_ra_write:
        PUT rA,r231
        POP 0,0
.size mmix_ra_write,.-mmix_ra_write
