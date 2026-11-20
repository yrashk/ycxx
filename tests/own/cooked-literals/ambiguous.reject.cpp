// [lex.ext], [over.match.best.general]: equally good cooked operator candidates are ambiguous.
namespace A { int operator""_k(unsigned long long); }
namespace B { int operator""_k(unsigned long long); }
using namespace A;
using namespace B;
int f() { return 2_k; } //! error
