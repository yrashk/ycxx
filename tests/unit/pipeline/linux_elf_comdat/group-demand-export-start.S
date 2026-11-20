.text
.globl _start
.type _start,@function
_start:
    call read_discarded_export
    movl %eax, %edi
    movl $60, %eax
    syscall
.size _start, .-_start
.section .note.GNU-stack,"",@progbits
