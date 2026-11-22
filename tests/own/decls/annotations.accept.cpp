// Class and function redeclarations retain distinct occurrences ([dcl.attr.annotation]).
// FLAGS: -freflection
// XFAIL: clang Clang 23 does not parse declaration annotations.
namespace fixture {
struct [[=7]] Record;
struct [[=7, =8]] Record {};
[[=4, =4]] void process();
void process [[=5]] ();
}
