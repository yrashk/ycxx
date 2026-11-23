struct Limits {
  bool read_later;
  int allowed = read_later ? requested : 7;
  int requested = 2;
};

template<bool ReadLater>
struct Profile {
  static constexpr int unrelated = 3;
  static constexpr int uses = Limits{ReadLater}.allowed;
};

static_assert(Profile<false>::uses == 7);
static_assert(Profile<true>::unrelated == 3);
