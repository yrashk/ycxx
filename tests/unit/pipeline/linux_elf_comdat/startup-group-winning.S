/* The selected group supplies a normal function but no entry point. */
.section .text.startup_group_winning,"axG",@progbits,startup_group_value,comdat
.globl startup_group_value
.type startup_group_value,@function
startup_group_value:
    movl $23, %eax
    ret
.size startup_group_value, .-startup_group_value
.section .note.GNU-stack,"",@progbits
