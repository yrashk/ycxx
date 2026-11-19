.text
.globl main
.type main, @function
main:
    # Unspecified bits may be nonzero in incoming register and stack arguments.
    subq $24, %rsp
    movabsq $0x9731552411228001, %rax
    movq %rax, (%rsp)
    movabsq $0x81d2b7e02a3344e7, %rax
    movq %rax, 8(%rsp)
    leaq narrow_registers(%rip), %rax
    movq %rax, 16(%rsp)
    movabsq $0x9876543212345681, %rdi
    movabsq $0x102030405060d9a7, %rsi
    xorl %edx, %edx
    xorl %ecx, %ecx
    movabsq $0xf0e0d0c0b0a0a45f, %r8
    movabsq $0xfedcba98765432e9, %r9
    call narrow_boundaries
    testl %eax, %eax
    jne .Ldone
    movl $0x12345681, %edi
    call narrow_return
    # The outgoing return is sign-extended through bit 31 for interoperability.
    cmpl $-127, %eax
    movl $6, %eax
    jne .Ldone
    xorl %eax, %eax
.Ldone:
    addq $24, %rsp
    ret
.size main, .-main

.globl narrow_registers
.type narrow_registers, @function
narrow_registers:
    # Observe the compatibility extension that optimized Clang callees depend on.
    cmpl $-127, %edi
    jne .Lbad_registers
    cmpl $0xd9a7, %esi
    jne .Lbad_registers
    cmpl $-32767, %r8d
    jne .Lbad_registers
    cmpl $0xe7, %r9d
    jne .Lbad_registers
    xorl %eax, %eax
    ret
.Lbad_registers:
    movl $5, %eax
    ret
.size narrow_registers, .-narrow_registers

.globl narrow_dirty_s16
.type narrow_dirty_s16, @function
narrow_dirty_s16:
    movabsq $0x8172635445368001, %rax
    ret
.size narrow_dirty_s16, .-narrow_dirty_s16

.globl narrow_dirty_u8
.type narrow_dirty_u8, @function
narrow_dirty_u8:
    movabsq $0x10203040506070e7, %rax
    ret
.size narrow_dirty_u8, .-narrow_dirty_u8

.section .note.GNU-stack,"",@progbits
