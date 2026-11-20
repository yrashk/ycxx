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
.globl discarded_export_reader
.type discarded_export_reader,@function
discarded_export_reader:
    movl discarded_export(%rip), %eax
    ret
.size discarded_export_reader, .-discarded_export_reader
