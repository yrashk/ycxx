/* ELF gABI §3.9 and Itanium C++ ABI §5.2.1. */
.section .text.select_group_a,"axG",@progbits,_Z12select_groupv,comdat
.globl _Z12select_groupv
.type _Z12select_groupv,@function
_Z12select_groupv:
    movq .Lpointer(%rip), %rax
    movl (%rax), %eax
    ret
.size _Z12select_groupv, .-_Z12select_groupv

.section .data.select_group_a,"awG",@progbits,_Z12select_groupv,comdat
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
    .long 23
.size group_value, .-group_value

.text
.globl from_a
.type from_a,@function
from_a:
    jmp _Z12select_groupv
.size from_a, .-from_a
.section .note.GNU-stack,"",@progbits
