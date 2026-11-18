// δ s4 (#86), from the enumerations' review: an implicit enumerator value that does not fit the
// fixed underlying type ([dcl.enum]/5).
enum E : unsigned char { a = 255, b }; //! error
int main() { return 0; }
