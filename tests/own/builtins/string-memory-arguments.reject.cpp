// __builtin_strchr, __builtin_strpbrk, __builtin_strrchr, __builtin_strstr, __builtin_memset,
// __builtin_memcpy and __builtin_memmove take exactly the C library's arguments.
using Size = decltype(sizeof(0));

char* search_with_no_value(const char* s) {
  return __builtin_strchr(s); //! error
}

char* search_with_extra_argument(const char* s) {
  return __builtin_strrchr(s, 'a', 1); //! error
}

char* search_in_int(int s) {
  return __builtin_strchr(s, 'a'); //! error
}

char* search_signed_chars(const signed char* s) {
  return __builtin_strrchr(s, 'a'); //! error
}

char* search_for_pointer(const char* s) {
  return __builtin_strchr(s, s); //! error
}

char* search_set_of_int(const char* s) {
  return __builtin_strpbrk(s, 'a'); //! error
}

char* search_text_of_one(const char* s) {
  return __builtin_strstr(s); //! error
}

void fill_with_no_length(char* d) {
  (void)__builtin_memset(d, 0); //! error
}

void fill_const(const char* d) {
  (void)__builtin_memset(d, 0, 1); //! error
}

void fill_volatile(volatile char* d) {
  (void)__builtin_memset(d, 0, 1); //! error
}

void copy_into_const(const char* d, const char* s) {
  (void)__builtin_memcpy(d, s, 1); //! error
}

void copy_from_volatile(char* d, volatile char* s) {
  (void)__builtin_memcpy(d, s, 1); //! error
}

void move_with_extra_argument(char* d, const char* s) {
  (void)__builtin_memmove(d, s, 1, 2); //! error
}

void move_into_const(const char* d, const char* s) {
  (void)__builtin_memmove(d, s, 1); //! error
}

void copy_with_pointer_length(char* d, const char* s) {
  (void)__builtin_memcpy(d, s, s); //! error
}

// The result is `void*`, which does not convert to `char*` implicitly.
char* copy_as_char(char* d, const char* s) {
  return __builtin_memcpy(d, s, 1); //! error
}

// The result is `char*`, which is not `const char*&`'s referent type.
void search_as_reference(const char* s) {
  const char*& r = __builtin_strchr(s, 'a'); //! error
  (void)r;
}
