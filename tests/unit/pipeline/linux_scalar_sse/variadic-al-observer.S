.text
.globl observe_none, observe_named, observe_many, observe_full
.type observe_none, @function
.type observe_named, @function
.type observe_many, @function
.type observe_full, @function
observe_none:
observe_named:
observe_many:
observe_full:
    movzbl %al, %eax
    ret
.size observe_none, .-observe_none
.size observe_named, .-observe_named
.size observe_many, .-observe_many
.size observe_full, .-observe_full
.section .note.GNU-stack,"",@progbits
