// #212: a class definition missing its ';' is reported at its closing brace, and what follows
// still parses ([class.pre]/1).
class a { public: int b; } //! error

int main() { a x{}; return x.b; }
