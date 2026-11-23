// [basic.scope.class], [class.member.lookup], [class.static.data], [basic.scope.pdecl]
constexpr int seed = 5;
using width = char;
struct Base {
  static constexpr int seed = 42;
  static constexpr int width = 0;
protected:
  static constexpr int permission = 11;
};
struct Inherited : Base {
  inline static int unqualified = seed;
  inline static int qualified = Base::seed;
  inline static int allowed = permission;
  inline static int bytes = sizeof(width);
};
struct Prior : Base {
  static constexpr int seed = 23;
  inline static int selected = seed;
};
struct Same : Base {
  inline static const int seed = 31, selected = seed;
};
struct SelfBase {
  static constexpr char self = 0;
};
struct Self : SelfBase {
  inline static int self = sizeof(self);
};

int main() {
  if (Inherited::unqualified != 42 || Inherited::qualified != 42) return 1;
  if (Inherited::allowed != 11 || Inherited::bytes != sizeof(int)) return 2;
  if (Prior::selected != 23 || Same::selected != 31) return 3;
  if (Self::self != sizeof(int)) return 4;
  return 0;
}
