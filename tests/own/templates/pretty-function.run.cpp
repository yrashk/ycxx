// #1361: template argument spellings are canonical, ordered and readable after return.
// EXIT: 0
template<class T>
const char* spelling() {
  return __PRETTY_FUNCTION__;
}

template<class First, class Second>
const char* sequence() {
  return __PRETTY_FUNCTION__;
}

bool same_text(const char* first, const char* second) {
  for (; *first == *second; ++first, ++second) {
    if (*first == '\0') return true;
  }
  return false;
}

namespace left { struct Item {}; }
namespace right { struct Item {}; }
using Alias = left::Item;
using FixedArray = int[2];
using IntFunction = void (*)(int);
using LongFunction = void (*)(long);
template<class T> struct Box {};

int main() {
  const char* first = spelling<left::Item>();
  const char* second = spelling<right::Item>();
  const char* again = spelling<Alias>();
  if (*first == '\0' || *second == '\0' || same_text(first, second)) return 1;
  if (!same_text(first, again)) return 2;

  if (same_text(spelling<int>(), spelling<unsigned int>())) return 3;
  if (same_text(spelling<int>(), spelling<const int>())) return 4;
  if (same_text(spelling<const int*>(), spelling<int* const>())) return 5;
  if (same_text(spelling<int&>(), spelling<int&&>())) return 6;
  if (same_text(spelling<FixedArray>(), spelling<int[3]>())) return 7;
  if (!same_text(spelling<FixedArray>(), spelling<int[2]>())) return 8;
  if (same_text(spelling<IntFunction>(), spelling<LongFunction>())) return 9;
  if (same_text(spelling<Box<left::Item>>(), spelling<Box<right::Item>>())) return 10;

  const char* forward = sequence<left::Item, long>();
  const char* reverse = sequence<long, left::Item>();
  if (same_text(forward, reverse)) return 11;
  if (!same_text(forward, sequence<Alias, long>())) return 12;
  return 0;
}
