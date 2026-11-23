// [basic.link], [dcl.inline]: a typed enum argument can make a specialization TU-local.
// EXIT: 0

namespace {
enum class LocalTag : unsigned { small = 11u };
}

enum class PublicTag : unsigned { large = 29u };

template<LocalTag Tag> struct PrivateBudget {
  unsigned charge() noexcept { return static_cast<unsigned>(Tag); }
};

template<PublicTag Tag> struct PublicBudget {
  unsigned charge() noexcept { return static_cast<unsigned>(Tag); }
};

int main() {
  PrivateBudget<LocalTag::small> local;
  PublicBudget<PublicTag::large> shared;
  return local.charge() != 11u || shared.charge() != 29u;
}
