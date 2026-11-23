// #822 [temp.local], [class.member.lookup], [temp.constr.atomic].
struct Threshold {
  static constexpr int limit = 3;
};

template<int Count>
struct Visible : Threshold {
  template<class T> requires (limit == 3)
  static int choose() { return 7; }
};

template<int limit>
struct Hidden : Threshold {
  template<class T> requires (limit == 3)
  static int choose() { return 11; }
};

int exercise() {
  return Visible<0>::choose<char>() + Hidden<0>::choose<char>();
}
