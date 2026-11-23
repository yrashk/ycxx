// [basic.link], [dcl.constexpr]: a typed enum argument can make a specialization TU-local.
// EXIT: 0

namespace {
enum class LocalTag : unsigned { small = 11u };
}

enum class PublicTag : unsigned { large = 29u };

template<LocalTag Tag> struct PrivateProfile {
  static constexpr unsigned limit = static_cast<unsigned>(Tag);
};

template<PublicTag Tag> struct PublicProfile {
  static constexpr unsigned limit = static_cast<unsigned>(Tag);
};

const unsigned* private_limit() noexcept {
  return &PrivateProfile<LocalTag::small>::limit;
}

const unsigned* public_limit() noexcept {
  return &PublicProfile<PublicTag::large>::limit;
}

int main() {
  const unsigned* local = private_limit();
  const unsigned* shared = public_limit();
  if (local == shared) return 1;
  return *local != 11u || *shared != 29u;
}
