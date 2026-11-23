    .section .rodata
    .globl weak_marker
    .type weak_marker, @object
weak_marker:
    .byte 41
    .size weak_marker, .-weak_marker
    .section .note.GNU-stack,"",@progbits
