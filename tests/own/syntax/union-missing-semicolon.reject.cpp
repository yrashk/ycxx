// #212: a union definition missing its ';' is reported at its closing brace, and what follows
// still parses ([class.pre]/1).
union a { int b; } //! error

int main() { a x{}; return x.b; }
