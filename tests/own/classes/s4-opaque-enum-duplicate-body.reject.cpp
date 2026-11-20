// Namespace-scope opaque enumerations ([dcl.enum], #157).
enum class E;
enum class E { x };
enum struct E { y }; //! error
