#include "group-b.S"

/* This is the only reference to ghost. It belongs to the selectable group. */
.section .data.select_group_b,"awG",@progbits,_Z12select_groupv,comdat
    .quad ghost
