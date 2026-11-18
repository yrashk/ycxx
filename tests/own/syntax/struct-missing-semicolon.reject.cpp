// #212: a struct definition missing its ';' is reported at its closing brace, and what follows
// still parses ([class.pre]/1).
struct a { int b; } //! error

int main() { a x{}; return x.b; }
