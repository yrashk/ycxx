using N = decltype(nullptr);

constexpr N n{};
static_assert(n == nullptr);
constexpr void* namespace_pointer = n;
static_assert(namespace_pointer == nullptr);

void check_block_and_parameter(N value) {
  // CWG 2907 permits the conversion without accessing the local object.
  N local;
  static_assert(local == nullptr);
  constexpr void* from_local = local;
  static_assert(from_local == nullptr);

  N initialized = nullptr;
  static_assert(initialized == nullptr);
  constexpr void* from_initialized = initialized;
  static_assert(from_initialized == nullptr);

  static_assert(value == nullptr);
  constexpr void* from_parameter = value;
  static_assert(from_parameter == nullptr);
}

constexpr bool check_self_initialization() {
  N self = self;
  constexpr void* pointer = self;
  return self == nullptr && pointer == nullptr;
}
static_assert(check_self_initialization());
