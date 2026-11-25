// #1361: the function-template signature is a static const character array.
template<class T>
constexpr bool valid_array() {
  static_assert(__is_same(decltype((__PRETTY_FUNCTION__)),
                          const char (&)[sizeof(__PRETTY_FUNCTION__)]));
  return sizeof(__PRETTY_FUNCTION__) > 1 && __PRETTY_FUNCTION__[0] != '\0' &&
         __PRETTY_FUNCTION__[sizeof(__PRETTY_FUNCTION__) - 1] == '\0';
}

static_assert(valid_array<int>());
static_assert(valid_array<long>());

template<class T>
consteval const char* spelling() {
  return __PRETTY_FUNCTION__;
}

constexpr bool same_text(const char* first, const char* second) {
  for (; *first == *second; ++first, ++second) {
    if (*first == '\0') return true;
  }
  return false;
}

namespace left { struct Item {}; }
namespace right { struct Item {}; }
using Alias = left::Item;

consteval bool retained_spellings() {
  const char* first = spelling<left::Item>();
  const char* second = spelling<right::Item>();
  const char* again = spelling<Alias>();
  return *first != '\0' && *second != '\0' && !same_text(first, second) &&
         same_text(first, again);
}

static_assert(retained_spellings());
