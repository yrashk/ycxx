#include "group-b.S"

/* There is no corresponding definition in the other group. */
.section .data.select_group_b,"awG",@progbits,_Z12select_groupv,comdat
.globl discarded_export
.type discarded_export,@object
discarded_export:
    .long 91
.size discarded_export, .-discarded_export

/* This ordinary text and relocation survive even when B loses. */
.text
.globl read_discarded_export
.type read_discarded_export,@function
read_discarded_export:
    movl discarded_export(%rip), %eax
    ret
.size read_discarded_export, .-read_discarded_export
