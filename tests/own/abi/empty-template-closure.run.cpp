// EXIT: 0

template<class T> struct Box {};
template<class T> Box<T> make_box() { return {}; }

auto pick() { return [](int value) { return value + 1; }; }

int main() {
  auto box = make_box<int>();
  return sizeof(box) == 1 && pick()(41) == 42 ? 0 : 1;
}
