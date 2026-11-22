// The peer selects lifetimes independently of ycxx's nontrivial union support.
struct Resource { int value; ~Resource() noexcept; };
struct Trivial { int value; };
extern int destroyed, receiver_calls;

void destroy_member(Resource&);
void destroy_const_member(const Resource*);
void destroy_trivial_member(Trivial*);

union Owned {
  Resource member;
  ~Owned() noexcept {}
};
union ConstOwned {
  const Resource member;
  ~ConstOwned() noexcept {}
};
union TrivialOwned { Trivial member; };

int peer() {
  Owned first{{3}};
  ConstOwned second{{7}};
  TrivialOwned third{{11}};
  destroy_member(first.member);
  if (destroyed != 3 || receiver_calls != 1) return 1;
  destroy_const_member(&second.member);
  if (destroyed != 10 || receiver_calls != 1) return 2;
  destroy_trivial_member(&third.member);
  return destroyed != 10 || receiver_calls != 2;
}
