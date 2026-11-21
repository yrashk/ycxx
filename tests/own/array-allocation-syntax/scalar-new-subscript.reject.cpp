// [expr.new], [expr.sub]: parenthesizing scalar new makes the brackets a subscript.
int* subscript(unsigned n) { return (new int)[n]; } //! error
