.text
.globl _start
.type _start,@function
_start:
    call discarded_export_reader
    movl %eax, %edi
    movl $60, %eax
    syscall
.size _start, .-_start
.section .note.GNU-stack,"",@progbits
