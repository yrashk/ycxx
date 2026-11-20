/* The entry point shares the duplicate group and must be discarded with it. */
.section .text.startup_group_losing,"axG",@progbits,startup_group_value,comdat
.globl startup_group_value
.type startup_group_value,@function
startup_group_value:
    movl $71, %eax
    ret
.size startup_group_value, .-startup_group_value

.section .text.startup_group_entry,"axG",@progbits,startup_group_value,comdat
.globl _start
.type _start,@function
_start:
    movl $99, %edi
    movl $60, %eax
    syscall
.size _start, .-_start
.section .note.GNU-stack,"",@progbits
