    .text

    .globl bool417_dirty_arguments
    .type bool417_dirty_arguments, @function
bool417_dirty_arguments:
    # The first and sixth arguments are in registers. The seventh and eighth
    # arguments occupy consecutive eightbyte slots. Only their low bytes
    # carry the bool values.
    subq $24, %rsp
    movabsq $0x8091929394959601, %rax
    movq %rax, 0(%rsp)
    movabsq $0xf0e1d2c3b4a59600, %rax
    movq %rax, 8(%rsp)
    movabsq $0x9a8b7c6d5e4f3a00, %rdi
    movl $22, %esi
    movl $33, %edx
    movl $44, %ecx
    movl $55, %r8d
    movabsq $0x0203040506070801, %r9
    call bool417_arguments
    addq $24, %rsp
    ret
    .size bool417_dirty_arguments, .-bool417_dirty_arguments

    .globl bool417_dirty_return
    .type bool417_dirty_return, @function
bool417_dirty_return:
    # Both results have nonzero excess bits and a canonical low byte.
    testl %edi, %edi
    jne .Ldirty_true
    movabsq $0xa1b2c3d4e5f60700, %rax
    ret
.Ldirty_true:
    movabsq $0x1020304050607001, %rax
    ret
    .size bool417_dirty_return, .-bool417_dirty_return

    .globl bool417_argument_bytes
    .type bool417_argument_bytes, @function
bool417_argument_bytes:
    cmpb $0, %dil
    jne .Lbad_argument_byte
    cmpb $1, %r9b
    jne .Lbad_argument_byte
    cmpb $1, 8(%rsp)
    jne .Lbad_argument_byte
    cmpb $0, 16(%rsp)
    jne .Lbad_argument_byte
    xorl %eax, %eax
    ret
.Lbad_argument_byte:
    movl $1, %eax
    ret
    .size bool417_argument_bytes, .-bool417_argument_bytes

    .globl bool417_return_bytes
    .type bool417_return_bytes, @function
bool417_return_bytes:
    subq $8, %rsp
    xorl %edi, %edi
    call bool417_return
    cmpb $0, %al
    jne .Lbad_false_return_byte
    movabsq $0x8000000000000000, %rdi
    call bool417_return
    cmpb $1, %al
    setne %al
    movzbl %al, %eax
    addq $8, %rsp
    ret
.Lbad_false_return_byte:
    movl $1, %eax
    addq $8, %rsp
    ret
    .size bool417_return_bytes, .-bool417_return_bytes

    .section .note.GNU-stack,"",@progbits
