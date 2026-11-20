// Namespace-scope opaque enumerations ([dcl.enum], #157).
enum class E : unsigned int;
enum struct E { x }; //! error
