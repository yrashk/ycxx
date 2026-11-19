.text
.globl verify_promotions
.type verify_promotions, @function
verify_promotions:
    # Check the vector count and stack alignment before the C peer's prologue.
    testb %al, %al
    jne .Lbad_nonempty_tail
    leaq 8(%rsp), %r11
    testq $15, %r11
    jne .Lbad_nonempty_tail
    jmp verify_variadic_values
.Lbad_nonempty_tail:
    movl $2, %eax
    ret
.size verify_promotions, .-verify_promotions

.globl verify_no_tail
.type verify_no_tail, @function
verify_no_tail:
    testb %al, %al
    jne .Lbad_empty_tail
    xorl %eax, %eax
    cmpl $-29, %edi
    setne %al
    ret
.Lbad_empty_tail:
    movl $3, %eax
    ret
.size verify_no_tail, .-verify_no_tail
.section .note.GNU-stack,"",@progbits
