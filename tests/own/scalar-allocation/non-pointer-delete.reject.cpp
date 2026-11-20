// [expr.delete]: an integer is not a pointer-to-object operand.
int main() { delete 1; } //! error
