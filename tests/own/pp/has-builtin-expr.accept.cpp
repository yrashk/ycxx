// `__has_builtin` expands in ordinary C++ expressions as well as `#if` conditions (#1626).
#ifndef __has_builtin
#error __has_builtin must be defined
#endif
#if !defined(__has_builtin)
#error defined(__has_builtin) must hold
#endif
#if !__has_builtin(__builtin_memcmp)
#error memcmp builtin is unavailable
#endif
#if __has_builtin(__ycxx_no_such_builtin)
#error unknown builtin reported available
#endif

static_assert(__has_builtin(__builtin_memcmp) == 1);
static_assert(__has_builtin(__ycxx_no_such_builtin) == 0);
static_assert(__has_builtin(int) == 0);

#define PROBE_BUILTIN(x) __has_builtin(x)
#define PASTE_NAME(a, b) a ## b
#define STRINGIZE(x) #x

static_assert(PROBE_BUILTIN(__builtin_memcmp) == 1);
static_assert(PROBE_BUILTIN(__ycxx_no_such_builtin) == 0);
static_assert(PASTE_NAME(__has_, builtin)(__builtin_memcmp) == 1);
static_assert(
    __has_builtin(
        __builtin_memcmp
    ) + 4 == 5);

constexpr const char* raw_probe = STRINGIZE(__has_builtin(__builtin_memcmp));
static_assert(raw_probe[0] == '_');

#if __has_embed(__FILE__ limit(__has_builtin(__builtin_memcmp))) != 1
#error __has_builtin inside __has_embed limit failed
#endif

constexpr int with_pragma = __has_builtin(
#pragma ycxx_has_builtin_probe
    __builtin_memcmp);
static_assert(with_pragma == 1);

