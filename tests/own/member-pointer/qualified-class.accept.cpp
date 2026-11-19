// [dcl.mptr] (#238)
namespace model {
struct Target;
struct Outer {
  using Inner = Target;
};
}

struct Global;

using NestedMember = int model::Outer::Inner::*;
extern NestedMember nested;
extern int ::model::Outer::Inner::* nested;

using GlobalMember = int Global::*;
extern GlobalMember global;
extern int ::Global::* global;
