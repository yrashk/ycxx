// Namespace-scope opaque enumerations ([dcl.enum], #157).
namespace left {
enum class State : unsigned short;
enum Tag : int;
}
namespace right {
enum class State;
}
static_assert(!__is_same(left::State, right::State));
static_assert(sizeof(left::State) == sizeof(unsigned short));
namespace left {
using Before = State;
using Base = unsigned short;
enum struct State : Base { ready = 42 };
enum Tag : int { active = 1 };
static_assert(__is_same(Before, State));
}
int main() { return static_cast<unsigned short>(left::State::ready) == 42 && left::active == 1 ? 0 : 1; }
