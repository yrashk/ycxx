// [stmt.if], [temp.inst]
namespace gate {

template<class T>
constexpr int helper(T value) {
  return value + 1;
}

template<class Tag>
constexpr int unused_bridge() {
  if consteval {
    return helper<int>(41);
  }
  return 0;
}

int runtime_entry(int value) {
  return helper<int>(value);
}

}

int main() {
  volatile int input = 41;
  return gate::runtime_entry(input) == 42 ? 0 : 1;
}
