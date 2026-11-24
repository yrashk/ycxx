    .text
    .globl _Z14witness_resultPbP17ResultObservationi
    .type _Z14witness_resultPbP17ResultObservationi, @function
_Z14witness_resultPbP17ResultObservationi:
    # witness_result(bool*, ResultObservation*, int) receives the result
    # address in RDI, followed by its three explicit arguments.
    subq $40, %rsp
    movq %rdi, 16(%rsp)
    movq %rsi, 24(%rsp)

    # Forward the incoming result address and supply make_result's arguments.
    movq %rdx, %rsi
    movl %ecx, %edx
    movl $301, %ecx
    movl $-407, %r8d
    movl $-53, %r9d
    movq $-16007, (%rsp)
    movabsq $0x123456789abcdef0, %r11
    movq %r11, 8(%rsp)
    call _Z11make_resultP17ResultObservationiiiasy

    # Report the provider's RAX echo, then return the result address ourselves.
    movq 24(%rsp), %r10
    cmpq 16(%rsp), %rax
    sete (%r10)
    movq 16(%rsp), %rax
    addq $40, %rsp
    ret
    .size _Z14witness_resultPbP17ResultObservationi, .-_Z14witness_resultPbP17ResultObservationi

    .section .note.GNU-stack,"",@progbits
