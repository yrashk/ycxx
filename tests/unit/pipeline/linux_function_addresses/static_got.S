    .text
#ifdef REQUIRE_MARKER
    .globl weak_marker
#else
    .weak weak_marker
#endif
    .type weak_marker, @object
    .globl _start
    .type _start, @function
_start:
    movq 0(%rip), %rax
    .reloc .-4, R_X86_64_REX_GOTPCRELX, weak_marker-4
    cmpq $0, 0(%rip)
    .reloc .-5, R_X86_64_GOTPCREL, weak_marker-5
    setne %dl
    leaq local_marker(%rip), %rcx
    cmpq 0(%rip), %rcx
    .reloc .-4, R_X86_64_GOTPCREL, local_marker-4
    jne .Lbad
    testb %dl, %dl
    je .Labsent
    testq %rax, %rax
    je .Lbad
    movzbl (%rax), %edi
    jmp .Lexit
.Labsent:
    testq %rax, %rax
    jne .Lbad
    xorl %edi, %edi
    jmp .Lexit
.Lbad:
    movl $99, %edi
.Lexit:
    movl $60, %eax
    syscall
    .size _start, .-_start

    .section .rodata
    .type local_marker, @object
local_marker:
    .byte 7
    .size local_marker, .-local_marker
    .section .note.GNU-stack,"",@progbits
