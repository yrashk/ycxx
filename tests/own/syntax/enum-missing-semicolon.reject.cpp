// #212: an enumeration definition missing its ';' is reported at its closing brace ([dcl.enum]/1).
enum a { b } //! error

int main() { return b; }
