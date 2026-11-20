    .text
    .weak missing_weak
    .type missing_weak, @function
    .globl main
    .type main, @function
main:
    movq 0(%rip), %rax
    .reloc .-4, R_X86_64_REX_GOTPCRELX, missing_weak-4
    testq %rax, %rax
    setne %al
    movzbl %al, %eax
    ret
    .size main, .-main
    .section .note.GNU-stack,"",@progbits
