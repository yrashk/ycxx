.text
.globl _start
.type _start,@function
_start:
    xorl %edi, %edi
    movl $60, %eax
    syscall
.size _start, .-_start

.data
.globl ordinary_duplicate
.type ordinary_duplicate,@object
ordinary_duplicate:
    .long 11
.size ordinary_duplicate, .-ordinary_duplicate
.section .note.GNU-stack,"",@progbits
