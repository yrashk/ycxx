// #813 [dcl.ambig.res], [stmt.ambig].
// EXIT: 0
struct Tag {
  int value;
  Tag(int n) : value(n) {}
};

struct Holder {
  int value;
  Holder(Tag first, Tag second) : value(first.value * 10 + second.value) {}
};

Holder named_parameters(Tag(a), Tag(b));
Holder adjusted_factory(Tag(), Tag());
static_assert(__is_same(decltype(named_parameters), Holder(Tag, Tag)));
static_assert(__is_same(decltype(adjusted_factory), Holder(Tag(*)(), Tag(*)())));

int main() {
  int n = 6;
  Holder later_add(Tag(n), Tag(n + 1));
  Holder later_subtract(Tag(n), Tag((n) - 1));
  Holder first_add(Tag(n + 1), Tag(n));
  return later_add.value != 67 || later_subtract.value != 65 || first_add.value != 76;
}
