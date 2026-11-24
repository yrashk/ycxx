// __builtin_memchr and __builtin_memcmp take exactly the C library's arguments.
using Size = decltype(sizeof(0));

void* find_with_no_length(const char* s) {
  return __builtin_memchr(s, 'a'); //! error
}

void* find_with_extra_argument(const char* s) {
  return __builtin_memchr(s, 'a', 1, 2); //! error
}

int compare_with_no_length(const char* a, const char* b) {
  return __builtin_memcmp(a, b); //! error
}

int compare_with_extra_argument(const char* a, const char* b) {
  return __builtin_memcmp(a, b, 1, 2); //! error
}

void* find_in_int(int s) {
  return __builtin_memchr(s, 'a', 1); //! error
}

void* find_in_volatile(volatile char* s) {
  return __builtin_memchr(s, 'a', 1); //! error
}

int compare_function(const char* a) {
  return __builtin_memcmp(a, "", Size{1} + a); //! error
}

void find_with_pointer_value(const char* s) {
  (void)__builtin_memchr(s, s, 1); //! error
}

// The result is `void*`, which does not convert to `const char*` implicitly.
const char* find_as_char(const char* s) {
  return __builtin_memchr(s, 'a', 1); //! error
}
