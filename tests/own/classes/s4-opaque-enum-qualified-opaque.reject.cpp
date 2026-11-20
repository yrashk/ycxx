// Namespace-scope opaque enumerations ([dcl.enum], #157).
namespace ns { enum class Code; }
enum class ns::Code; //! error
