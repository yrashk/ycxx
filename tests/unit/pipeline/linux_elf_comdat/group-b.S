/* Same group signature and different section names. */
.section .text.select_group_b,"axG",@progbits,_Z12select_groupv,comdat
.globl _Z12select_groupv
.type _Z12select_groupv,@function
_Z12select_groupv:
    movq .Lpointer(%rip), %rax
    movl (%rax), %eax
    ret
.size _Z12select_groupv, .-_Z12select_groupv

.section .data.select_group_b,"awG",@progbits,_Z12select_groupv,comdat
.p2align 3
.globl cohort_pointer
.type cohort_pointer,@object
cohort_pointer:
.Lpointer:
    .quad group_value
.size cohort_pointer, .-cohort_pointer
.local group_value
.type group_value,@object
group_value:
    .long 58
.size group_value, .-group_value

.text
.globl from_b
.type from_b,@function
from_b:
    jmp _Z12select_groupv
.size from_b, .-from_b
/* This group has the same ELF section-header name and a different signature. */
.section .text.independent_probe,"axG",@progbits,independent_probe,comdat
.globl independent_probe
.type independent_probe,@function
independent_probe:
    movl $7, %eax
    ret
.size independent_probe, .-independent_probe
.section .note.GNU-stack,"",@progbits
