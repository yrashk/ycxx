struct S {
private:
  using T = int;
  typedef int Legacy;
public:
  using Public = int;

  static int from_member(int value) {
    return S::T(value) + S::T{value} + S::Legacy(value);
  }

  static int call(int value) { return value + 1; }
  friend int from_friend(int value);
};

int from_friend(int value) {
  return S::T(value) + S::T{value} + S::Legacy{value};
}

namespace Exported {
  using T = S::Public;
}
namespace Alias = Exported;

int outside(int value) {
  return S::Public(value) + S::Public{value} + Exported::T(value)
      + Alias::T{value} + S::call(value) + S::from_member(value)
      + from_friend(value);
}
