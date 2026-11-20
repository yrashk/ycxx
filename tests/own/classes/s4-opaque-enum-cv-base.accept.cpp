// Namespace-scope opaque enumerations ([dcl.enum], #157).
using Integer = const int;
enum class Code : Integer;
enum struct Code : volatile int;
enum class Code { ok = 42 };
static_assert(sizeof(Code) == sizeof(int));
int main() { return static_cast<int>(Code::ok) - 42; }
