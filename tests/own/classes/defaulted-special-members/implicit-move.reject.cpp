// #157, #244 [class.copy.ctor]: ycxx does not move a class implicitly where a member's defaulted move is
// its own operation, and does not copy instead: the member's copy has another effect.
// XFAIL: clang,gcc valid: an implicit move that ycxx does not synthesize yet (#244)
struct Member { Member(); Member(const Member&); Member(Member&&) = default; int x; };
struct Holder { Member m; };
void move(Holder& h) { Holder to(static_cast<Holder&&>(h)); } //! error
