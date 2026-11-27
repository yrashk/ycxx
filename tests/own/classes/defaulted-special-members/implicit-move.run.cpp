// #244 [class.copy.ctor]/8, 14: an implicit move constructor moves a member by its own defaulted move,
// not by its copy. implicit-move.reject.cpp checks that ycxx says it cannot, and does not copy.
// XFAIL: ycxx implicit moves of such classes are not synthesized yet (#244)
// EXIT: 0
int copies = 0;
struct Member {
  int x;
  Member() : x(1) {}
  Member(const Member& m) : x(m.x) { ++copies; }
  Member(Member&&) = default;
};
struct Holder { Member m; };

int main() {
  Holder h;
  Holder to(static_cast<Holder&&>(h));
  return copies == 0 && to.m.x == 1 ? 0 : 1;
}
