/* System V AMD64 psABI §§3.2.3 and 3.5.7. */
.text
.globl indirect_variadic_seed_al
.type indirect_variadic_seed_al, @function
indirect_variadic_seed_al:
    /* The wrapper is nonvariadic, so its incoming %al is unconstrained. */
    movl $77, %eax
    jmp indirect_variadic_empty
.size indirect_variadic_seed_al, .-indirect_variadic_seed_al

.p2align 8
.byte 0x90
.globl indirect_variadic_read_al
.type indirect_variadic_read_al, @function
indirect_variadic_read_al:
    /* The address's low byte is 1, so a target left in %rax is detectable. */
    movzbl %al, %eax
    shll $8, %eax
    orl %edi, %eax
    ret
.size indirect_variadic_read_al, .-indirect_variadic_read_al

.globl indirect_variadic_check_al
.type indirect_variadic_check_al, @function
indirect_variadic_check_al:
    /* Preserve the variable arguments and the return address. */
    testb %al, %al
    jne .Lnonzero_al
    jmp indirect_variadic_receive
.Lnonzero_al:
    movl $7, %eax
    ret
.size indirect_variadic_check_al, .-indirect_variadic_check_al
.section .note.GNU-stack,"",@progbits
