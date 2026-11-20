.text
.globl _start
.type _start,@function
_start:
    call from_a
    movl %eax, %r12d
    movl $111, %edi
    call from_b
    cmpl %r12d, %eax
    jne .Lexit
    movl $112, %edi
    call _Z12select_groupv
    cmpl %r12d, %eax
    jne .Lexit
    movl $113, %edi
    movq cohort_pointer(%rip), %rax
    cmpl %r12d, (%rax)
    jne .Lexit
    movl $114, %edi
    call independent_probe
    cmpl $7, %eax
    jne .Lexit
    movl %r12d, %edi
.Lexit:
    movl $60, %eax
    syscall
.size _start, .-_start
.section .note.GNU-stack,"",@progbits
