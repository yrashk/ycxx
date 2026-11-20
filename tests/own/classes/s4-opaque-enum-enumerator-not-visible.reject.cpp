// Namespace-scope opaque enumerations ([dcl.enum], #157).
enum class E;
int use_before_definition() { return static_cast<int>(E::Later); } //! error
enum class E { Later = 1 };
