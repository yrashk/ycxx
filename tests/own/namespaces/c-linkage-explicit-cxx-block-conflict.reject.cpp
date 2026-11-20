// An enclosing linkage block specifies the redeclaration's language linkage ([dcl.link]).
namespace n { extern "C" int ycxx_lookup(int); }
extern "C++" { int n::ycxx_lookup(int v) { return v; } } //! error
