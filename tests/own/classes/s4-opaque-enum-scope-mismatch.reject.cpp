// Namespace-scope opaque enumerations ([dcl.enum], #157).
enum class M : unsigned;
enum M : unsigned { one }; //! error
