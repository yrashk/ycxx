// #822 [temp.local], [class.member.lookup], [temp.over.link].
struct Threshold {
  static constexpr int limit = 3;
};

template<int limit>
struct Selection : Threshold {
  template<class T> requires (limit == 3)
  static int choose();
};

template<int limit>
template<class T> requires (limit == 3)
int Selection<limit>::choose() { return 7; } //! error
