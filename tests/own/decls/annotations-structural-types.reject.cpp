// Annotation payloads must have structural types ([dcl.attr.annotation], [meta.reflection.result]).
// FLAGS: -freflection
namespace fixture {
struct VolatileMember { volatile int value; };
struct RuntimeDestructor { int value; ~RuntimeDestructor(); };
struct RvalueReferenceMember { int&& value; };
int target;

[[=VolatileMember{1}]] void volatile_member();  //! error
[[=RuntimeDestructor{2}]] void runtime_destructor();  //! error
[[=RvalueReferenceMember{static_cast<int&&>(target)}]] void rvalue_reference_member();  //! error
}
