"""Exercise compatibility modes in cold, resident, and editor compilations."""

import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

from lsp import Editor, expect
from process import Env, failures
from system_includes import DARWIN, LINUX, NATIVE_LINUX


MODES = ('spec', 'gcc', 'clang')
ENUM_WIDTH = '''\
enum Wide { high = 0xffffffffffffffffULL, increment, following = increment + 1 };
#ifdef CLANG_MODE
static_assert(sizeof(Wide) == 8);
static_assert(__is_same(__underlying_type(Wide), unsigned long));
static_assert(__is_same(decltype(+increment), unsigned long));
static_assert(increment == 0 && following == 1);
#else
static_assert(sizeof(Wide) == 16);
static_assert(__is_same(__underlying_type(Wide), unsigned __int128));
static_assert(__is_same(decltype(+increment), __int128));
static_assert(increment == (__int128(1) << 64));
static_assert(following == (__int128(1) << 64) + 1);
#endif

enum Explicit { explicit_wide = ((unsigned __int128)1 << 127), explicit_next };
#ifdef CLANG_MODE
static_assert(__is_same(__underlying_type(Explicit), unsigned long long));
static_assert(__is_same(decltype(+explicit_wide), long long));
static_assert(static_cast<unsigned long long>(explicit_wide) == 0);
static_assert(static_cast<unsigned long long>(explicit_next) == 1);
#else
static_assert(__is_same(__underlying_type(Explicit), unsigned __int128));
static_assert(__is_same(decltype(+explicit_wide), unsigned __int128));
static_assert(static_cast<unsigned __int128>(explicit_wide) == ((unsigned __int128)1 << 127));
static_assert(static_cast<unsigned __int128>(static_cast<Explicit>(((unsigned __int128)1 << 127) + 113)) ==
              ((unsigned __int128)1 << 127) + 113);
#endif

enum Fixed : unsigned __int128 { fixed_high = 0xffffffffffffffffULL, fixed_increment };
static_assert(static_cast<unsigned __int128>(fixed_increment) == ((unsigned __int128)1 << 64));

extern "C" Wide wide_enum(Wide value, unsigned long long key);
#ifdef PROVIDER
Wide wide_enum(Wide value, unsigned long long key) {
  return value == increment && key == 17 ? following : high;
}
#elif defined(CONSUMER)
int main() { return wide_enum(increment, 17) != following; }
#endif
'''
TAIL_PADDING = '''\
#if defined(DEFAULTED_COPY)
struct Base { int value; char byte; Base(const Base&) = default; };
#elif defined(DELETED_COPY)
struct Base { int value; char byte; Base(const Base&) = delete; };
#elif defined(DELETED_DESTRUCTOR)
struct Base { int value; char byte; ~Base() = delete; };
#elif defined(ASSIGNMENT_TEMPLATE)
struct Base { int value; char byte; template<class T> Base& operator=(T); };
#elif defined(PRIVATE_MEMBERS)
struct Base { private: int value; char byte; };
#elif defined(OVERLAPPING_MEMBER)
struct Base { int value; [[no_unique_address]] char byte; };
#else
struct Base { int value; char byte; };
#endif
struct Derived : Base { char tail; };
#if defined(EXPECTED)
static_assert(sizeof(Derived) == EXPECTED);
#endif
'''
CLASS_HEAD_ALIGNMENT = '''\
#if defined(WEAK)
struct alignas(1) Payload { long long value; };
static_assert(alignof(Payload) == 8);
#elif defined(WEAK_AFTER_ZERO)
struct alignas(0) Payload;
struct alignas(1) Payload { long long value; };
#ifdef GCC_MODE
static_assert(alignof(Payload) == 8);
#endif
#elif defined(OPAQUE)
struct alignas(8) Payload;
struct alignas(16) Payload;
Payload* pointer;
#elif defined(OPAQUE_ZERO)
struct alignas(0) Payload;
struct alignas(8) Payload;
Payload* pointer;
#elif defined(MISSING)
struct alignas(16) Payload;
struct Payload { char value; };
static_assert(alignof(Payload) == 16);
#elif defined(REPLACE)
struct alignas(16) Payload;
struct alignas(8) Payload { char value; };
static_assert(alignof(Payload) == 8);
#elif defined(FORWARD_CHAIN)
struct alignas(16) Payload;
struct alignas(8) Payload;
struct alignas(0) Payload { char value; };
static_assert(alignof(Payload) == 8);
#elif defined(ZERO_FIRST)
struct alignas(0) Payload;
struct alignas(16) Payload { long long value; };
static_assert(alignof(Payload) == 16);
#elif defined(ZERO_ON_DEFINITION)
struct alignas(8) Payload;
struct alignas(0) Payload { long long value; };
static_assert(alignof(Payload) == 8);
#elif defined(ZERO_LATER)
struct alignas(16) Payload { char value; };
struct alignas(0) Payload;
static_assert(alignof(Payload) == 16);
#elif defined(LATE_STRONGER)
struct Payload { char value; };
struct alignas(16) Payload;
static_assert(alignof(Payload) == 1);
#else
struct alignas(16) alignas(8) alignas(0) Payload { char value; };
#ifdef GCC_MODE
static_assert(alignof(Payload) == 8);
#else
static_assert(alignof(Payload) == 16);
#endif
#endif
'''
CLASS_HEAD_ALIGNMENT_CASES = {
    'WEAK': ('gcc',), 'WEAK_AFTER_ZERO': ('gcc', 'clang'),
    'OPAQUE': ('spec', 'gcc'), 'OPAQUE_ZERO': ('spec', 'gcc'), 'MISSING': ('gcc',), 'REPLACE': ('gcc',),
    'FORWARD_CHAIN': ('gcc',), 'ZERO_FIRST': ('gcc', 'clang'),
    'ZERO_ON_DEFINITION': ('spec', 'gcc'), 'ZERO_LATER': ('gcc', 'clang'),
    'LATE_STRONGER': ('gcc',),
}
FUNCTION_LANGUAGE_LINKAGE = '''\
extern "C" typedef int CFunction(int);
using CxxFunction = int(int);
#ifdef SPEC_MODE
static_assert(!__is_same(CFunction, CxxFunction));
#else
static_assert(__is_same(CFunction, CxxFunction));
#endif
#if defined(OVERLOAD)
int pick(CFunction*) { return 1; }
int pick(CxxFunction*) { return 2; }
#elif defined(CONFLICT)
extern "C" int conflict(int);
extern "C++" int conflict(int);
#elif defined(QUALIFIED_CONFLICT)
namespace N { extern "C" int conflict(int); }
extern "C++" int N::conflict(int) { return 0; }
#else
extern "C" int named(int);
int named(int n) { return n * 2; }
CFunction declared;
int declared(int n) { return n + 1; }
static_assert(__is_same(decltype(named), CFunction));
static_assert(__is_same(decltype(declared), CFunction));
namespace N { CFunction declare_in_namespace; }
int N::declare_in_namespace(int n) { return n + 2; }
static_assert(__is_same(decltype(N::declare_in_namespace), CFunction));
int consume(CFunction* function) { return function(31); }
#endif
'''
WIDE_ARRAY = '''\
using Count = decltype(sizeof(0));
extern "C" Count observed();
char* allocate() {
#if defined(TOO_LARGE)
  return new char[((unsigned __int128)1 << 64) + (1ul << 63)];
#elif defined(NEGATIVE)
  return new char[1 - ((__int128)1 << 64)];
#elif defined(UNSIGNED_WIDE)
  return new char[((unsigned __int128)1 << 64) + 2];
#elif defined(ZERO)
  return new char[((__int128)1 << 64)];
#elif defined(CONTROL)
  return new char[1];
#else
  return new char[((__int128)1 << 64) + 1];
#endif
}
int main() {
  return allocate() == nullptr || observed() != EXPECTED;
}
'''
ABSTRACT_RESULTS = '''\
struct Abstract {
  virtual int score(int) = 0;
#if defined(STATIC_SELF)
  static Abstract object;
#endif
};
Abstract make();
using Result = decltype(make());
static_assert(__is_same(Result, Abstract));
#if defined(CALL)
void discard() { make(); }
#elif defined(COMMA)
using Comma = decltype((make(), 0));
static_assert(__is_same(Comma, int));
#elif defined(VOID_CAST)
using Discarded = decltype(static_cast<void>(make()));
static_assert(__is_same(Discarded, void));
#elif defined(RECEIVER)
using MemberCall = decltype(make().score(1));
static_assert(__is_same(MemberCall, int));
#elif defined(SIZEOF)
static_assert(sizeof(make()) > 0);
#elif defined(EXTERN_OBJECT)
extern Abstract object;
#elif defined(PARAMETER)
Abstract& source();
void consume(Abstract);
using Argument = decltype(consume(source()));
#endif
'''
EMPTY_UNION = '''\
int constructions;
struct Resource {
  int value;
#if defined(EXPLICIT)
  explicit Resource() noexcept : value(7) { ++constructions; }
#else
  Resource() noexcept : value(7) { ++constructions; }
#endif
  Resource(const Resource&) = delete;
  ~Resource() noexcept {}
};
union Storage {
  Resource first;
  int fallback;
  ~Storage() noexcept {}
};
struct Holder { Storage storage; };
union WithDefault { int first = 19; char fallback; };
#if defined(ALLOCATE)
union ScalarFirst { int first; Resource next; ~ScalarFirst() noexcept {} };
int main() {
  Storage* allocated = new Storage{};
  ScalarFirst* scalar = new ScalarFirst{};
  int result = constructions != 1 || allocated->first.value != 7 || scalar->first != 0;
  delete allocated;
  delete scalar;
  return result;
}
#elif defined(ALLOCATE_CONTROL)
union WithResourceDefault { int first = 17; Resource next; ~WithResourceDefault() noexcept {} };
int main() {
  WithResourceDefault* allocated = new WithResourceDefault{};
  int result = constructions != 0 || allocated->first != 17;
  delete allocated;
  return result;
}
#else
int main() {
  Storage direct{};
  Holder omitted{};
  Holder explicit_list{{}};
  WithDefault with_default{};
  if (with_default.first != 19) return 1;
#if defined(GCC_MODE)
  return constructions != 0;
#else
  return constructions != 3 || direct.first.value != 7 || omitted.storage.first.value != 7 ||
         explicit_list.storage.first.value != 7;
#endif
}
#endif
'''
PACK_INDEX_NAMES = '''\
namespace N {
inline constexpr unsigned zero = 0;
static constexpr int hidden = 0;
enum Pos { first };
}
struct S { static constexpr unsigned int zero = 0; };
'''
PACK_INDEX_FUNCTIONS = (
    ('var', 'N::zero', 'L_ZN1N4zeroEE'),
    ('hidden', 'N::hidden', 'L_ZN1NL6hiddenEE'),
    ('member', 'S::zero', 'L_ZN1S4zeroEE'),
    ('enu', 'N::first', 'LN1N3PosE0E'),
    ('sum', '0 + 0', 'plLi0ELi0E'),
    ('neg', '-0', 'ngLi0E'),
    ('cast', 'static_cast<unsigned>(0)', 'scjLi0E'),
    ('nested', '(0 + 1) - 1', 'miplLi0ELi1ELi1E'),
    ('enum_sum', 'N::first + 0', 'plLN1N3PosE0ELi0E'),
)
OPERATOR_CONSTRAINTS = (
    ('template<class T> requires (sizeof(T) > 0) int size(T)', 'size(0)', '_Z4sizeIiQgtstT_Li0EEiS0_'),
    ('template<class T> requires (alignof(T) == 4) int alignment(T)', 'alignment(0)', '_Z9alignmentIiQeqatT_Li4EEiS0_'),
    ('template<unsigned N> requires (N + 1 > 2) int numeric()', 'numeric<3>()', '_Z7numericILj3EQgtplT_Li1ELi2EEiv'),
    ('template<class T> int trailing(T) requires (sizeof(T) != 3)', 'trailing(0)', '_Z8trailingIiEiT_QnestS0_Li3E'),
    ('template<class T> requires (static_cast<bool>(sizeof(T))) int cast(T)', 'cast(0)', '_Z4castIiQscbstT_EiS0_'),
    ('template<class T> requires ((sizeof(T) & 1u) == 0) int bits(T)', 'bits(0)', '_Z4bitsIiQeqanstT_Lj1ELi0EEiS0_'),
)
PREFIX_TEMPLATES = '''\
template<class> struct One {};
template<class, class = int> struct Two {};
template<class...> struct Pack {};
'''
PREFIX_FUNCTIONS = (
    ('template<template<class> class... TT> int multi()', 'multi<>()', '_Z5multiIJEEiv', '_Z5multiITpTtTyEJEEiv'),
    ('', 'multi<One>()', '_Z5multiIJ3OneEEiv', '_Z5multiIJ3OneEEiv'),
    ('', 'multi<One, Two>()', '_Z5multiIJ3One3TwoEEiv', '_Z5multiIJ3One3TwoEEiv'),
    ('', 'multi<Two, One>()', '_Z5multiITpTtTyEJ3Two3OneEEiv', '_Z5multiITpTtTyEJ3Two3OneEEiv'),
    ('', 'multi<One, Pack>()', '_Z5multiIJ3One4PackEEiv', '_Z5multiIJ3One4PackEEiv'),
    ('template<template<class> class TT> int one()', 'one<Pack>()', '_Z3oneITtTyE4PackEiv', '_Z3oneITtTyE4PackEiv'),
    ('template<class T, T... Ns> int val()', 'val<int>()', '_Z3valIiJEEiv', '_Z3valIiTpTnT_JEEiv'),
    ('', 'val<int, 4>()', '_Z3valIiJLi4EEEiv', '_Z3valIiTpTnT_JLi4EEEiv'),
    ('template<int... Ns> int integral()', 'integral<>()', '_Z8integralIJEEiv', '_Z8integralITpTniJEEiv'),
    ('', 'integral<4>()', '_Z8integralIJLi4EEEiv', '_Z8integralIJLi4EEEiv'),
)
LINE = '''\
constexpr unsigned reported(unsigned value = __builtin_LINE()) { return value; }
#define INVOKE() reported()
#define LINE() __LINE__
#ifdef GCC_MODE
using Line = int;
#else
using Line = unsigned;
#endif
static_assert(__is_same(decltype(__builtin_LINE()), Line));
#line 100
constexpr auto written = reported
  (
  );
#line 200
constexpr auto expanded = INVOKE(
);
#line 300
constexpr auto preprocessing = LINE(
);
#ifdef GCC_MODE
static_assert(written == 101 && expanded == 200 && preprocessing == 300);
#else
static_assert(written == 100 && expanded == 201 && preprocessing == 301);
#endif
'''
DESCRIPTOR = '''\
#include <source_location>
#line 100
constexpr auto location = std::source_location::current
  (
  );
#ifdef GCC_MODE
static_assert(location.line() == 101 && location.column() == 3);
#else
static_assert(location.line() == 100 && location.column() == 27);
#endif
extern "C" constexpr auto recorded_location = location;
'''
MANGLE = '''\
template<bool> struct Box { int n; };
namespace N { template<class, int = 7> constexpr bool value = true; }
template<class T> int variable(Box<N::value<T>>) { return 1; }
template<class... Ts> int index(Ts...[0]) { return 2; }
template<class T, T V> int typed() { return V; }
template<class T, T... V> int empty_values() { return sizeof...(V); }
template<auto V> int placeholder() { return V; }
template<auto... V> int empty_placeholders() { return sizeof...(V); }
template<auto V> struct pw {};
template<auto... V> struct pp {};
namespace pn { template<auto V> int scoped() { return V; } }
template<template<auto> class TT> int auto_head() { return 6; }
template<template<int> class TT> int int_head() { return 7; }
template<template<auto...> class TT> int pack_head() { return 8; }
template<class, class = int> struct Two {};
template<template<class> class T> int different_head() { return 5; }
template<class T> struct Y { template<class> using apply = T; };
template<template<class> class Q> struct Use {};
template<class T> int member(Use<Y<T>::template apply>) { return 3; }
int use() {
  return variable<int>(Box<true>{0}) + index<int>(0) + typed<int, 4>()
       + member<int>(Use<Y<int>::apply>{}) + empty_values<int>() + different_head<Two>()
       + placeholder<4>() + empty_placeholders<>() + pn::scoped<4>() + auto_head<pw>() + int_head<pw>()
       + pack_head<pp>();
}
int target() noexcept { return 4; }
using Signature = int (*)();
constexpr Signature function = &target;
template<Signature F> int converted() { return F(); }
int pointer() { return converted<function>(); }
template<class T> inline constexpr auto lt = [](T n) { return n + 2; };
int closure(int x) {
  int (*fn)(int) = [](int n) { return n + 1; };
  return fn(x) + lt<int>(x);
}
'''
SYMBOLS = {
    'variable': ('_Z8variableIiEi3BoxIXsr1NE5valueIT_EEE', '_Z8variableIiEi3BoxIX5valueIT_Li7EEEE'),
    'index': ('_Z5indexIJiEEiDyT_Lm0E', '_Z5indexIJiEEiDyT_Li0E'),
    'typed': ('_Z5typedIiTnT_Li4EEiv', '_Z5typedIiLi4EEiv'),
    'empty-values': ('_Z12empty_valuesIiTpTnT_JEEiv', '_Z12empty_valuesIiJEEiv'),
    'empty-placeholders': ('_Z18empty_placeholdersITpTnDaJEEiv', '_Z18empty_placeholdersIJEEiv'),
    # GCC writes no <template-param-decl> after a <nested-name>, and Tt before a template whose
    # head has a placeholder parameter even when the heads are the same.
    'scoped-placeholder': ('_ZN2pn6scopedITnDaLi4EEEiv', '_ZN2pn6scopedILi4EEEiv'),
    'auto-head': ('_Z9auto_headI2pwEiv', '_Z9auto_headITtTnDaE2pwEiv'),
    'pack-head': ('_Z9pack_headI2ppEiv', '_Z9pack_headITtTpTnDaE2ppEiv'),
    'member': ('_Z6memberIiEi3UseIN1YIT_E5applyEE', '_Z6memberIiEi3UseIsr1YIT_EE5applyE'),
    'closure': ('_ZZ7closureiENUliE_8__invokeEi', '_ZZ7closureiENUliE_4_FUNEi'),
    'closure-prefix': ('_ZNK2ltIiEMUliE_clEi', '_ZNK2ltIiEUliE_clEi'),
    'function-pointer': ('_Z9convertedIXcvPFivEadL_Z6targetvEEEiv', '_Z9convertedIXadL_Z6targetvEEEiv'),
}
CLASS_DECLARATIONS = '''\
struct Immovable {
  int n;
  Immovable(int value) : n(value) {}
  Immovable(const Immovable&) = delete;
};
struct Member { Immovable value; int tail; };
struct Array { Immovable values[2]; int tail; };
int peer_member(Member, int);
int peer_array(Array);
int ycxx_member(Member, int);
int ycxx_array(Array);
Member result_abi(int);
int check_peer();
'''
CLASS_CALLER = '''\
int ycxx_member(Member m, int extra) { return m.value.n + m.tail + extra; }
int ycxx_array(Array m) { return m.values[0].n + m.values[1].n + m.tail; }
Member result_abi(int tail) { return Member{Immovable(10), tail}; }
int main() {
  if (peer_member(Member{Immovable(10), 2}, 4) != 16) return 1;
  if (peer_array(Array{{Immovable(10), Immovable(20)}, 2}) != 32) return 2;
  return check_peer();
}
'''
CLASS_PEER = '''\
int peer_member(Member m, int extra) { return m.value.n + m.tail + extra; }
int peer_array(Array m) { return m.values[0].n + m.values[1].n + m.tail; }
int check_peer() {
  if (ycxx_member(Member{Immovable(10), 3}, 4) != 17) return 3;
  if (ycxx_array(Array{{Immovable(10), Immovable(20)}, 3}) != 33) return 4;
  if (result_abi(3).tail != 3) return 5;
  return 0;
}
'''
LARGE_CLASS_DECLARATIONS = CLASS_DECLARATIONS + '''\
struct Large { Immovable first; int words[16]; int tail; };
int ycxx_large(int, Large, int, int, int, int, int, int);
int peer_large(int, Large, int, int, int, int, int, int);
Large ycxx_large_result(int);
Large peer_large_result(int);
int test_large_peer();
'''
LARGE_CLASS_CALLER = '''\
int ycxx_large(int a, Large value, int b, int c, int d, int e, int f, int g) {
  return a + b + c + d + e + f + g + value.first.n + value.words[0] + value.words[1] + value.words[15] + value.tail;
}
Large ycxx_large_result(int tail) { return Large{Immovable(7), {3, 11}, tail}; }
int main() {
  if (peer_large(1, peer_large_result(9), 2, 3, 4, 5, 6, 7) != 58) return 1;
  return test_large_peer() != 58;
}
'''
LARGE_CLASS_PEER = '''\
int peer_large(int a, Large value, int b, int c, int d, int e, int f, int g) {
  return a + b + c + d + e + f + g + value.first.n + value.words[0] + value.words[1] + value.words[15] + value.tail;
}
Large peer_large_result(int tail) { return Large{Immovable(7), {3, 11}, tail}; }
int test_large_peer() { return ycxx_large(1, ycxx_large_result(9), 2, 3, 4, 5, 6, 7); }
'''
CONCEPTS = '''\
template<class T> concept Always = true;
template<class T> concept Never = false;
namespace N { template<class T, class U = char> concept Check = true; }
struct Marker { using type = int; };
'''
CONSTRAINED_FUNCTIONS = (
    'template<Always T> int short_form(T t)',
    'template<class T> requires (Always<T> && !Never<T>) int head(T t)',
    'template<class T> int trailing(T t) requires (Always<T> || Never<T>)',
    'template<class T> requires N::Check<T> int qualified(T t)',
    'template<class T> requires requires { typename T::type; } int type_req(T t)',
    'template<bool B> requires B int bare()',
)
CONSTRAINED_CALLS = 'short_form(1) + head(1) + trailing(1) + qualified(1) + type_req(Marker{}) + bare<true>()'


def preprocess_and_syntax(world):
    def invoke(*args, status=0):
        args = [LINUX, *args]
        cold = world.cxx(args, world.scope, world.cold, timeout=30)
        warm = world.cxx(args, world.scope, world.daemon, timeout=30)
        assert cold == warm and warm[0] == status, (args, cold, warm)
        return warm

    (world.scope / 'version.cpp').write_text('__cplusplus\n')
    (world.scope / 'class-head-alignment.cpp').write_text(CLASS_HEAD_ALIGNMENT)
    (world.scope / 'tail-padding.cpp').write_text(TAIL_PADDING)
    (world.scope / 'clang-under-alignment.cpp').write_text('''\
struct alignas(0) Aligned;
struct alignas(WIDTH) Aligned { long long member; };
#ifdef GCC_MODE
static_assert(sizeof(Aligned) == 8 && alignof(Aligned) == 8);
static_assert(sizeof(Aligned[2]) == 16);
#else
static_assert(sizeof(Aligned) == WIDTH && alignof(Aligned) == WIDTH);
static_assert(sizeof(Aligned[2]) == 2 * WIDTH);
#endif
static_assert(__is_same(decltype(Aligned::member), long long));
#if defined(OBJECT)
Aligned object;
#elif defined(LOCAL)
void local() { Aligned value; }
#elif defined(MEMBER)
long long read(Aligned* value) { return value->member; }
#elif defined(COPY)
void copy(Aligned* to, Aligned* from) { *to = *from; }
#elif defined(PTR_ADD)
Aligned* advance(Aligned* value) { return value + 1; }
#elif defined(PTR_DIFF)
long difference(Aligned* a, Aligned* b) { return a - b; }
#elif defined(BY_VALUE)
void consume(Aligned value) {}
#elif defined(OFFSET)
static_assert(__builtin_offsetof(Aligned, member) == 0);
#elif defined(CONTAINS)
struct Holder { Aligned object; long long later; };
static_assert(sizeof(Holder) == 16);
#else
int report_size() { return sizeof(Aligned); }
Aligned* pointer;
#endif
''')
    (world.scope / 'wide-array.cpp').write_text(WIDE_ARRAY)
    (world.scope / 'abstract-results.cpp').write_text(ABSTRACT_RESULTS)
    (world.scope / 'line.cpp').write_text(LINE)
    (world.scope / 'descriptor.cpp').write_text(DESCRIPTOR)
    (world.scope / 'immediate-escape.cpp').write_text('''\
consteval int value(int n) { return n; }
#ifdef GLOBAL_POINTER
constexpr auto escaped = value;
#elif defined(GLOBAL_REFERENCE)
int (&escaped)(int) = (0, value);
#elif defined(LOCAL_POINTER)
consteval int call() { constexpr auto p = &value; return p(42); }
static_assert(call() == 42);
#elif defined(LOCAL_REFERENCE)
void call() { constexpr int (&p)(int) = value; }
#else
consteval int consume(int (*p)(int)) { return p(42); }
static_assert(consume(&value) == 42);
constexpr int ordinary = (&value)(42);
static_assert(ordinary == 42);
#endif
''')
    (world.scope / 'dependent-template.cpp').write_text('''\
struct Owner { template<class T> using Alias = T; };
template<template<class> class> struct Carrier {};
#ifdef TEMPLATE_ARGUMENT
template<class Q> using Result = Carrier<Q::Alias>;
static_assert(__is_same(Result<Owner>, Carrier<Owner::Alias>));
#elif defined(EXPLICIT_TEMPLATE)
template<class Q> using Result = typename Q::template Alias<int>;
#else
template<class Q> using Result = typename Q::Alias<int>;
#endif
#ifndef TEMPLATE_ARGUMENT
static_assert(__is_same(Result<Owner>, int));
#endif
''')
    (world.scope / 'alias-identity.cpp').write_text('''\
template<template<class...> class> struct Identity {};
template<class T> struct Original {};
template<class T> using Forward = Original<T>;
template<class T> using Nested = Forward<T>;
template<class T> using Transform = Original<T*>;
template<class T> requires true using Constrained = Original<T>;
template<class... T> struct Variadic {};
template<class... T> using ForwardPack = Variadic<T...>;
template<class T = int> struct Defaulted {};
template<class T = int> using SameDefault = Defaulted<T>;
template<class T = long> using DifferentDefault = Defaulted<T>;
struct Owner { template<class T> using Member = Original<T>; };
template<class Q> using Selected = Identity<Q::template Member>;
static_assert(!__is_same(Identity<Original>, Identity<Transform>));
static_assert(!__is_same(Identity<Original>, Identity<Constrained>));
static_assert(!__is_same(Identity<Defaulted>, Identity<DifferentDefault>));
#ifdef GCC_MODE
static_assert(__is_same(Identity<Original>, Identity<Forward>));
static_assert(__is_same(Identity<Original>, Identity<Nested>));
static_assert(__is_same(Identity<Variadic>, Identity<ForwardPack>));
static_assert(__is_same(Identity<Defaulted>, Identity<SameDefault>));
static_assert(__is_same(Identity<Original>, Identity<Owner::Member>));
static_assert(__is_same(Identity<Original>, Selected<Owner>));
#else
static_assert(!__is_same(Identity<Original>, Identity<Forward>));
static_assert(!__is_same(Identity<Original>, Identity<Nested>));
static_assert(!__is_same(Identity<Variadic>, Identity<ForwardPack>));
static_assert(!__is_same(Identity<Defaulted>, Identity<SameDefault>));
static_assert(!__is_same(Identity<Original>, Identity<Owner::Member>));
static_assert(!__is_same(Identity<Original>, Selected<Owner>));
#endif
''')
    (world.scope / 'memchr.cpp').write_text('static_assert(__builtin_memchr("abc", \'a\', 10) != nullptr);\n')
    (world.scope / 'memcmp.cpp').write_text('static_assert(__builtin_memcmp("abc", "bbc", 10) < 0);\n')
    (world.scope / 'memcmp-identity.cpp').write_text('''\
#if defined(NULL_ZERO)
static_assert(__builtin_memcmp(nullptr, nullptr, 0) == 0);
#elif defined(NULL_RANGE)
static_assert(__builtin_memcmp(nullptr, nullptr, 8) == 0);
#elif defined(INT_RANGE)
constexpr int object = 12;
static_assert(__builtin_memcmp(&object, &object, sizeof(object)) == 0);
#elif defined(PAST_END)
constexpr char object[] = "a";
static_assert(__builtin_memcmp(object + 2, object + 2, 8) == 0);
#elif defined(LOCAL)
constexpr int compare() {
  char object[] = {1, 2};
  return __builtin_memcmp(object, object, 8);
}
static_assert(compare() == 0);
#else
constexpr char object[] = "a";
static_assert(__builtin_memcmp(object, object, 123) == 0);
#endif
''')
    (world.scope / 'memcmp-integer.cpp').write_text('''\
#if defined(INT_ARRAY)
constexpr int left[] = {7, 256}, right[] = {7, 1};
static_assert(__builtin_memcmp(left, right, sizeof(left)) < 0);
#elif defined(INT128)
constexpr unsigned __int128 left = (unsigned __int128)1 << 64, right = (unsigned __int128)2 << 64;
static_assert(__builtin_memcmp(&left, &right, sizeof(left)) < 0);
#else
constexpr unsigned left = 256, right = 1;
static_assert(__builtin_memcmp(&left, &right, sizeof(left)) < 0);
#endif
''')
    (world.scope / 'memchr-integer.cpp').write_text('''\
constexpr unsigned num = 0x12131415;
constexpr unsigned arr[] = {0x12131415, 0x30313233};
#ifdef OVERSIZED
static_assert(__builtin_memchr(&num, 0x15, sizeof(num) + 1) == &num);
#else
constexpr const void* middle = __builtin_memchr(&num, 0x14, sizeof(num));
constexpr const void* later = __builtin_memchr(&num, 0x13, sizeof(num));
static_assert(__builtin_memchr(&num, 0x15, sizeof(num)) == &num);
static_assert(middle != nullptr && middle != &num);
static_assert(__builtin_memchr(&num, 0x14, 2) == middle);
static_assert(__builtin_memchr(&num, 0x18, sizeof(num)) == nullptr);
static_assert(__builtin_memchr(arr, 0x33, sizeof(arr)) == arr + 1);
static_assert(__builtin_memcmp(middle, middle, 123) == 0);
static_assert(__builtin_memcmp(middle, later, 1) > 0);
#endif
''')
    (world.scope / 'memory-array-object.cpp').write_text('''\
constexpr char first[2][2] = {{1, 2}, {3, 4}}, second[2][2] = {{1, 2}, {3, 5}};
constexpr unsigned ints[2] = {1, 2}, more[2] = {1, 3};
#if defined(OVERSIZED)
constexpr int bad = __builtin_memcmp(&first, &second, sizeof(first) + 1);
#elif defined(ROW_OVERSIZED)
constexpr int bad = __builtin_memcmp(first[0] + 2, first[1], 3);
#elif defined(RECORD_FIELDS)
struct Fields { char begin[2]; char end[2]; };
constexpr Fields fields = {{1, 2}, {3, 4}};
constexpr int bad = __builtin_memcmp(fields.begin + 2, fields.end, 1);
#else
static_assert(__builtin_memcmp(&first, &second, sizeof(first)) < 0);
static_assert(__builtin_memcmp(&first[1], &second[1], sizeof(first[1])) < 0);
static_assert(__builtin_memcmp(&ints, &more, sizeof(ints)) < 0);
static_assert(__builtin_memchr(&first, 4, sizeof(first)) == &first[1][1]);
static_assert(__builtin_memchr(&ints, 2, sizeof(ints)) == &ints[1]);
static_assert(__builtin_memcmp(first[0] + 2, first[1], 2) == 0);
static_assert(__builtin_memcmp(first[1], first[0] + 2, 2) == 0);
static_assert(__builtin_memcmp(&first, &first[0], 2) == 0);
static_assert(__builtin_memcmp(&first, first[0], 2) == 0);
static_assert(__builtin_memcmp(&first[0], first[0], 123) == 0);
static_assert(__builtin_memcmp(&ints, ints, 123) == 0);
#endif
''')
    (world.scope / 'memory-scalar-object.cpp').write_text('''\
#ifdef FLOAT
constexpr float left = 1.0f, right = 1.000000119f;
static_assert(__builtin_memcmp(&left, &right, sizeof(left)) < 0);
static_assert(__builtin_memchr(&left, 0x80, sizeof(left)) != nullptr);
#elif defined(DOUBLE)
constexpr double left[] = {1.0}, right[] = {2.0};
static_assert(__builtin_memcmp(&left, &right, sizeof(left)) > 0);
static_assert(__builtin_memchr(left, 0x3f, sizeof(left)) != nullptr);
#else
enum Bits : unsigned { low = 0x31, high = 0x32 };
constexpr Bits left = low, right = high;
static_assert(__builtin_memcmp(&left, &right, sizeof(left)) < 0);
static_assert(__builtin_memchr(&left, 0x31, sizeof(left)) == &left);
#endif
''')
    (world.scope / 'memory-record-object.cpp').write_text('''\
struct Padded { char tag; unsigned word; };
struct Tail { unsigned word; char tag; };
struct WithArray { char tag; unsigned words[2]; };
constexpr Padded low{3, 0x44332211}, equal{3, 0x44332211}, high{3, 0x44332212};
#if defined(REUSE)
constexpr const void* padding = __builtin_memchr(&low, 0, sizeof low);
static_assert(__builtin_memcmp(padding, &equal, 1) < 0);
#elif defined(LOCAL)
constexpr bool local() {
  constexpr Padded a{3, 4}, b{3, 5};
  return __builtin_memcmp(&a, &b, sizeof a) < 0;
}
static_assert(local());
#elif defined(ARRAY)
constexpr Padded records[]{low, high};
constexpr WithArray with_array{3, {1, 2}};
static_assert(__builtin_memchr(&records, 0x12, sizeof records) == &records[1].word);
static_assert(__builtin_memchr(&with_array, 2, sizeof with_array) == &with_array.words[1]);
#else
constexpr Tail tail{0x44332211, 3};
static_assert(__builtin_memcmp(&low, &equal, sizeof low) == 0);
static_assert(__builtin_memcmp(&low, &high, sizeof low) < 0);
constexpr const void* padding = __builtin_memchr(&low, 0, sizeof low);
static_assert(padding != nullptr && padding != &low);
static_assert(__builtin_memcmp(padding, padding, 50) == 0);
static_assert(__builtin_memchr(&low, 3, sizeof low) == &low);
static_assert(__builtin_memchr(&low, 0x11, sizeof low) == &low.word);
static_assert(__builtin_memchr(&tail, 0, sizeof tail) != nullptr);
#endif
''')
    (world.scope / 'memory-pointer-object.cpp').write_text('''\
#ifdef NON_NULL
int object;
constexpr int* pointer = &object;
static_assert(__builtin_memchr(&pointer, 0, sizeof pointer) != nullptr);
#elif defined(LOCAL)
constexpr bool local() {
  int* pointer = nullptr;
  return __builtin_memchr(&pointer, 0, sizeof pointer) == &pointer;
}
static_assert(local());
#else
constexpr int* left = nullptr;
constexpr int* right = nullptr;
constexpr auto literal = nullptr;
constexpr int* array[2] = {nullptr, nullptr};
static_assert(__builtin_memcmp(&left, &right, sizeof left) == 0);
static_assert(__builtin_memchr(&left, 0, sizeof left) == &left);
static_assert(__builtin_memchr(&left, 1, sizeof left) == nullptr);
static_assert(__builtin_memchr(&literal, 0, sizeof literal) == &literal);
static_assert(__builtin_memchr(&array, 0, sizeof array) == &array);
#endif
''')
    (world.scope / 'void-cast-nested.cpp').write_text('''\
constexpr int table[2][2] = {{17, 23}, {83, 97}};
#ifdef LAST
constexpr const void* erased = table[1] + 2;
#else
constexpr const void* erased = table[0] + 2;
#endif
constexpr const int* end = static_cast<const int*>(erased);
#if defined(TERMINAL_REENTER)
constexpr auto advanced = end + 2;
static_assert(advanced - 2 == end);
static_assert(end - table[1] == 2);
#elif defined(TERMINAL_BACKWARD)
constexpr auto before_end = end - 1;
static_assert(before_end == table[1] + 1);
#elif defined(TERMINAL_READ)
static_assert(end[-1] == 97);
#elif defined(READ_NEXT)
static_assert(*end == 83);
#elif defined(READ_PREVIOUS)
static_assert(end[-1] == 23);
#elif defined(LAST)
static_assert(end == table[1] + 2);
#else
static_assert(end == table[1]);
#endif
''')
    (world.scope / 'constexpr-pointer-bounds.cpp').write_text('''\
constexpr int flat[2] = {13, 37};
constexpr int table[2][2] = {{13, 37}, {83, 109}};
constexpr int singleton = 139;
#if defined(READ_PAST)
constexpr int value = *(flat + 3);
#elif defined(READ_WRONG_ROW)
constexpr int value = *(table[0] + 3);
#elif defined(DIFF_WRONG_ROW)
static_assert((table[0] + 3) - (table[1] + 1) == 0);
#elif defined(DOWNCAST)
constexpr auto erased = static_cast<const void*>(flat + 3);
constexpr auto pointer = static_cast<const int*>(erased);
#elif defined(TERMINAL_CAST)
constexpr auto address = static_cast<const int*>(static_cast<const void*>(table[1] + 2));
static_assert((address + 1) == (address + 1));
#else
constexpr auto past = flat + 3;
constexpr auto before = flat - 1;
constexpr auto singleton_past = &singleton + 2;
constexpr auto wrong_row = table[0] + 3;
static_assert(past - before == 4 && before < past);
static_assert(past == flat + 3 && before == flat - 1);
static_assert(*(past - 2) == 37 && *(before + 2) == 37);
static_assert(*(singleton_past - 2) == 139);
static_assert(wrong_row == table[1] + 1);
static_assert(table[1] - 1 == table[0] + 1);
static_assert(__builtin_memcmp(past, past, 5) == 0);
#endif
''')
    (world.scope / 'nan.cpp').write_text('constexpr float value = __builtin_nanf("") + 1.0f;\n')
    (world.scope / 'void-cast.cpp').write_text('''\
#ifdef SINGLETON
constexpr int value = 37;
constexpr const void* erased = &value + 1;
#else
constexpr int matrix[2][2] = {{13, 37}, {83, 109}};
constexpr const void* erased = matrix[0] + 2;
#endif
#ifdef UNRELATED
constexpr const float* end = static_cast<const float*>(erased);
#else
constexpr const int* end = static_cast<const int*>(erased);
#endif
#ifdef READ_PREVIOUS
static_assert(end[-1] == 37);
#endif
''')
    (world.scope / 'overflow.cpp').write_text('constexpr float value = __FLT_MAX__ * 2.0f;\n')
    (world.scope / 'warning.cpp').write_text("int value = 'ab';\n")
    (world.scope / 'macro.cpp').write_text('#define __INT64_C(c) c ## L\nstatic_assert(__INT64_C(4) == 4L);\n')
    headers = world.scope / 'includes'
    headers.mkdir()
    (headers / 'constant.h').write_text('#define __INT64_C(c) c ## L\n')
    (world.scope / 'system-macro.cpp').write_text('#include <constant.h>\nstatic_assert(__INT64_C(4) == 4L);\n')
    (world.scope / 'layout.cpp').write_text('''\
struct Base { private: int n; char c; };
struct Derived : Base { char next; };
static_assert(sizeof(Derived) == EXPECTED);
''')
    (world.scope / 'member-alignment.cpp').write_text('''\
struct alignas(16) Aligned { int a; };
struct Natural { int a; };
struct A { Aligned a; };
struct B { Natural a; };
struct C { alignas(16) Natural a; };
struct D { alignas(int) Natural a; };
union U { alignas(16) int a; };
union V { int a; };
static_assert(__is_layout_compatible(U, V));
#ifdef CLANG_MODE
static_assert(__is_layout_compatible(A, B));
static_assert(!__is_layout_compatible(A, C));
static_assert(!__is_layout_compatible(B, D));
#else
static_assert(!__is_layout_compatible(A, B));
static_assert(__is_layout_compatible(A, C));
static_assert(__is_layout_compatible(B, D));
#endif
''')
    (world.scope / 'incomplete-self.cpp').write_text('''\
struct Incomplete;
union Union;
struct Complete {};
static_assert(!__builtin_is_virtual_base_of(Union, Union));
static_assert(!__builtin_is_virtual_base_of(Complete, Complete));
static_assert(__is_pointer_interconvertible_base_of(Incomplete, Incomplete));
#ifdef INCOMPLETE
static_assert(!__builtin_is_virtual_base_of(const Incomplete, Incomplete));
#endif
''')
    (world.scope / 'embed.bin').write_bytes(bytes((13, 255, 65)))
    (world.scope / 'attribute-splice.cpp').write_text('''\
struct Record {
#ifdef UNMATCHED_OPENER
  [[probe::annotation([: unavailable_name)]] int value;
#elif defined(UNMATCHED_CLOSER)
  [[probe::annotation(unavailable_name :])]] int value;
#else
  [[probe::annotation([: unavailable_name :])]] int value;
#endif
};
''')
    (world.scope / 'attribute-delimiters.cpp').write_text('''\
struct S {
#if defined(BRACE)
  [[probe::annotation({ name)]] int value;
#elif defined(BRACKET)
  [[probe::annotation([ name)]] int value;
#elif defined(PAREN)
  [[probe::annotation(( name)]] int value;
#elif defined(RIGHT_BRACE)
  [[probe::annotation(} name)]] int value;
#elif defined(RIGHT_BRACKET)
  [[probe::annotation(] name)]] int value;
#elif defined(RIGHT_PAREN)
  [[probe::annotation(name))]] int value;
#elif defined(CROSSED_BRACE)
  [[probe::annotation(({ name ) })]] int value;
#elif defined(CROSSED_BRACKET)
  [[probe::annotation([{ name ] })]] int value;
#elif defined(KNOWN_ATTRIBUTE)
  [[nodiscard({ name)]] int value;
#elif defined(MISSING_OUTER)
  [[probe::annotation(name]] int value;
#elif defined(UNQUALIFIED)
  [[annotation({ name)]] int value;
#elif defined(ATTR_LIST)
  [[probe::annotation({ name), maybe_unused]] int value;
#else
  [[probe::annotation([name[2]])]] int value;
#endif
};
static_assert(sizeof(S) == sizeof(int));
''')
    offset_spellings = (('spec', 'offset'), ('gcc', 'gnu::offset'), ('gcc', '__gnu__::__offset__'),
                        ('clang', 'clang::offset'))
    for i, (_, spelling) in enumerate(offset_spellings):
        (world.scope / f'offset-{i}.cpp').write_text(f'''\
#if (__has_embed("embed.bin" {spelling}(2)) == __STDC_EMBED_FOUND__) != EXPECTED
#error incorrect offset support
#endif
#if EXPECTED || defined(FORCE_DIRECTIVE)
constexpr int byte =
#embed "embed.bin" {spelling}(1) limit(1)
;
static_assert(byte == 255);
#endif
''')
    (world.scope / 'function-template-pack.cpp').write_text('''\
#ifndef VALID_ONLY
template<class... A, class... B> void two_packs() {}
template<class... A, class B> void missing() {}
#endif
template<class... A, class B> void deduce(B) {}
template<class... A, class B = int> void with_default() {}
void check() {
#ifndef VALID_ONLY
  two_packs();
  two_packs<int>();
#endif
  deduce<int>(42);
  with_default<int>();
#ifdef MISSING
  missing<int>();
#endif
}
''')
    (world.scope / 'ref-qualified.cpp').write_text('''\
struct Receiver {
  int f();
  int f() &;
  operator int() { return 3; }
  operator int() & { return 4; }
};
int Receiver::f() { return 1; }
int Receiver::f() & { return 2; }
int use(Receiver& receiver) {
#ifdef LVALUE_CALL
  return receiver.f();
#else
  return Receiver{}.f();
#endif
}
#ifdef STATIC_MEMBER
struct Static { static int f(); int f() &; };
#endif
''')
    (world.scope / 'addressof-pack.cpp').write_text('''\
template<class... Ts> int* address(Ts&... args) { return __builtin_addressof(args...); }
#ifdef TWO_WRITTEN
template<class... Ts> int* address_twice(int& first, Ts&... rest) {
  return __builtin_addressof(first, rest...);
}
#endif
#ifdef INSTANTIATE
int* forward_address(int& value) { return address(value); }
#endif
''')
    comparison = Path(__file__).resolve().parents[1] / 'own/three-way-comparison/inputs/compare.hpp'
    shutil.copyfile(comparison, world.scope / 'compare.hpp')
    (world.scope / 'spaceship.cpp').write_text('''\
#include "compare.hpp"
struct Order {
  int n;
  constexpr Order(std::strong_ordering o) noexcept : n(o < 0 ? -1 : o > 0 ? 1 : 0) {}
#ifndef ONLY_UNEQUAL
  friend constexpr bool operator==(Order& o, int) noexcept { return o.n == 0; }
#endif
  friend constexpr bool operator!=(Order& o, int) { return o.n != 0; }
};
struct Pair { int a, b; Order operator<=>(const Pair&) const = default; };
#ifdef GCC_MODE
static_assert(noexcept(Pair{} <=> Pair{}));
#else
static_assert(!noexcept(Pair{} <=> Pair{}));
#endif
static_assert((Pair{0, 1} <=> Pair{0, 2}).n == -1);
static_assert((Pair{0, 4} <=> Pair{0, 4}).n == 0);
''')
    (world.scope / 'alias-adl.cpp').write_text('''\
template<template<class> class T> struct Carrier {};
namespace original {
template<class T> struct Class {};
int direct(Carrier<Class>);
template<class T> int through_original(T);
}
namespace exposed {
template<class T> using Forward = original::Class<T>;
template<class T> using Transform = original::Class<T*>;
template<class T> int through_alias(T);
}
int control(Carrier<original::Class> value) { return direct(value); }
#ifndef CONTROL_ONLY
#ifdef FORWARD
using Arg = Carrier<exposed::Forward>;
#else
using Arg = Carrier<exposed::Transform>;
#endif
#ifdef ORIGINAL
int lookup(Arg value) { return through_original(value); }
#else
int lookup(Arg value) { return through_alias(value); }
#endif
#endif
''')
    (world.scope / 'array-cv.cpp').write_text('''\
void check() {
  const int a[1][2] = {{1, 2}};
  volatile int b[1] = {3};
  auto [row] = a;
  const auto [element] = b;
#if defined(GCC_MODE) || defined(CLANG_MODE)
  static_assert(__is_same(decltype(row), const int[2]));
  static_assert(__is_same(decltype(element), const volatile int));
#else
  static_assert(__is_same(decltype(row), int[2]));
  static_assert(__is_same(decltype(element), const int));
#endif
#ifdef MUTATE
  row[0] = 4;
#endif
}
''')
    (world.scope / 'trait-symbol.cpp').write_text('''\
#ifdef IN_BODY
template<class T> auto transform(T n) { using U = __remove_cv(T); return U(n); }
#else
template<class T> __remove_cv(T) transform(T n) { return n; }
#endif
int use() { return transform(7); }
''')
    default = invoke('-E', 'version.cpp')
    assert b'202603L' in default[1], default
    pid = world.pid()

    for mode in (*MODES, 'gcc', 'spec'):
        flags = ['-fcompat=' + mode]
        expected = b'202603L' if mode == 'spec' else b'202400L'
        assert expected in invoke(*flags, '-E', 'version.cpp')[1]
        if mode == 'gcc':
            flags.append('-DGCC_MODE')
        if mode == 'clang':
            flags.append('-DCLANG_MODE')
        for target in (LINUX, DARWIN):
            for case in ('', 'NEGATIVE', 'UNSIGNED_WIDE', 'ZERO', 'CONTROL', 'TOO_LARGE'):
                case_flags = ['-D' + case] if case else []
                rejected = case == 'TOO_LARGE' or (mode != 'clang' and case != 'CONTROL')
                _, _, diagnostic = invoke(target, *flags, *case_flags, '-DEXPECTED=1', '-fsyntax-only',
                                          'wide-array.cpp', status=rejected)
                assert (b'changes its value' in diagnostic) == (not rejected and case != 'CONTROL'), (
                    target, mode, case, diagnostic)
            _, _, diagnostic = invoke(target, *flags, '-Wno-constant-conversion', '-DEXPECTED=1', '-fsyntax-only',
                                      'wide-array.cpp', status=mode != 'clang')
            assert b'changes its value' not in diagnostic, (target, mode, diagnostic)
            invoke(target, *flags, '-fsyntax-only', 'abstract-results.cpp')
            for case in ('CALL', 'COMMA', 'VOID_CAST', 'RECEIVER', 'SIZEOF'):
                invoke(target, *flags, '-D' + case, '-fsyntax-only', 'abstract-results.cpp', status=mode != 'clang')
            for case in ('STATIC_SELF', 'EXTERN_OBJECT'):
                invoke(target, *flags, '-D' + case, '-fsyntax-only', 'abstract-results.cpp', status=mode == 'clang')
            invoke(target, *flags, '-DPARAMETER', '-fsyntax-only', 'abstract-results.cpp', status=1)
            if mode == 'clang':
                # Materializing the call's result is observable when emitting the function body.
                invoke(target, *flags, '-DCALL', '-c', 'abstract-results.cpp', '-o', 'abstract-results.o')
        for target in (LINUX, DARWIN):
            darwin_clang = target == DARWIN and mode == 'clang'
            sizes = {
                'DEFAULTED_COPY': 8 if darwin_clang else 12,
                'DELETED_COPY': 8 if darwin_clang else 12,
                'DELETED_DESTRUCTOR': 12,
                'ASSIGNMENT_TEMPLATE': 12,
                'PRIVATE_MEMBERS': 12 if darwin_clang else 8,
                'OVERLAPPING_MEMBER': 8 if mode == 'gcc' else 12,
            }
            for case, size in sizes.items():
                invoke(target, *flags, '-D' + case, f'-DEXPECTED={size}', '-fsyntax-only', 'tail-padding.cpp')
        invoke(*flags, '-fsyntax-only', 'line.cpp')
        invoke(*flags, '-fsyntax-only', 'descriptor.cpp')
        for target in (LINUX, DARWIN):
            invoke(target, *flags, '-c', 'descriptor.cpp', '-o', 'descriptor.o')
            symbol = b'_recorded_location' if target == DARWIN else b'recorded_location'
            assert symbol in (world.scope / 'descriptor.o').read_bytes().split(b'\0'), (target, mode)
        invoke(*flags, '-fsyntax-only', 'immediate-escape.cpp')
        for case in ('GLOBAL_POINTER', 'GLOBAL_REFERENCE', 'LOCAL_POINTER', 'LOCAL_REFERENCE'):
            invoke(*flags, f'-D{case}', '-fsyntax-only', 'immediate-escape.cpp', status=mode != 'spec')
        invoke(*flags, '-DEXPLICIT_TEMPLATE', '-fsyntax-only', 'dependent-template.cpp')
        invoke(*flags, '-fsyntax-only', 'dependent-template.cpp', status=mode != 'gcc')
        invoke(*flags, '-DTEMPLATE_ARGUMENT', '-fsyntax-only', 'dependent-template.cpp', status=1)
        invoke(*flags, '-fsyntax-only', 'alias-identity.cpp')
        invoke(*flags, '-fsyntax-only', 'memchr.cpp', status=mode == 'gcc')
        invoke(*flags, '-fsyntax-only', 'memcmp.cpp', status=mode != 'clang')
        for target in (LINUX, DARWIN):
            invoke(target, *flags, '-DNULL_ZERO', '-fsyntax-only', 'memcmp-identity.cpp')
            for case in ('', 'NULL_RANGE', 'INT_RANGE', 'PAST_END', 'LOCAL'):
                case_flags = ['-D' + case] if case else []
                invoke(target, *flags, *case_flags, '-fsyntax-only', 'memcmp-identity.cpp', status=mode != 'gcc')
            for case in ('', 'INT_ARRAY', 'INT128'):
                case_flags = ['-D' + case] if case else []
                invoke(target, *flags, *case_flags, '-fsyntax-only', 'memcmp-integer.cpp', status=mode != 'gcc')
            invoke(target, *flags, '-fsyntax-only', 'memchr-integer.cpp', status=mode != 'gcc')
            invoke(target, *flags, '-DOVERSIZED', '-fsyntax-only', 'memchr-integer.cpp', status=1)
            invoke(target, *flags, '-fsyntax-only', 'memory-array-object.cpp', status=mode != 'gcc')
            for case in ('OVERSIZED', 'ROW_OVERSIZED', 'RECORD_FIELDS'):
                invoke(target, *flags, '-D' + case, '-fsyntax-only', 'memory-array-object.cpp', status=1)
            for case in ('FLOAT', 'DOUBLE', 'ENUM'):
                invoke(target, *flags, '-D' + case, '-fsyntax-only', 'memory-scalar-object.cpp', status=mode != 'gcc')
            for case in ('', 'ARRAY', 'REUSE', 'LOCAL'):
                args = ['-D' + case] if case else []
                invoke(target, *flags, *args, '-fsyntax-only', 'memory-record-object.cpp',
                       status=mode != 'gcc' or case in ('REUSE', 'LOCAL'))
            for case in ('', 'NON_NULL', 'LOCAL'):
                args = ['-D' + case] if case else []
                invoke(target, *flags, *args, '-fsyntax-only', 'memory-pointer-object.cpp',
                       status=mode != 'gcc' or bool(case))
            invoke(target, *flags, '-fsyntax-only', 'void-cast-nested.cpp', status=mode == 'spec')
            invoke(target, *flags, '-DREAD_PREVIOUS', '-fsyntax-only', 'void-cast-nested.cpp', status=mode != 'clang')
            invoke(target, *flags, '-DREAD_NEXT', '-fsyntax-only', 'void-cast-nested.cpp', status=mode != 'gcc')
            invoke(target, *flags, '-DLAST', '-fsyntax-only', 'void-cast-nested.cpp', status=mode == 'spec')
            invoke(target, *flags, '-DLAST', '-DTERMINAL_REENTER', '-fsyntax-only', 'void-cast-nested.cpp',
                   status=mode != 'gcc')
            for case in ('TERMINAL_BACKWARD', 'TERMINAL_READ'):
                invoke(target, *flags, '-DLAST', '-D' + case, '-fsyntax-only', 'void-cast-nested.cpp',
                       status=mode != 'clang')
            invoke(target, *flags, '-fsyntax-only', 'constexpr-pointer-bounds.cpp', status=mode != 'gcc')
            invoke(target, *flags, '-DTERMINAL_CAST', '-fsyntax-only', 'constexpr-pointer-bounds.cpp', status=mode != 'gcc')
            for rejected in ('READ_PAST', 'READ_WRONG_ROW', 'DIFF_WRONG_ROW', 'DOWNCAST'):
                invoke(target, *flags, '-D' + rejected, '-fsyntax-only', 'constexpr-pointer-bounds.cpp', status=1)
        invoke(*flags, '-fsyntax-only', 'nan.cpp', status=mode != 'gcc')
        invoke(*flags, '-fsyntax-only', 'void-cast.cpp', status=mode == 'spec')
        invoke(*flags, '-DSINGLETON', '-fsyntax-only', 'void-cast.cpp', status=mode != 'clang')
        invoke(*flags, '-DUNRELATED', '-fsyntax-only', 'void-cast.cpp', status=1)
        if mode == 'clang':
            invoke(*flags, '-DREAD_PREVIOUS', '-fsyntax-only', 'void-cast.cpp')
        invoke(*flags, '-fsyntax-only', 'overflow.cpp', status=mode == 'gcc')
        diagnostic = invoke(*flags, '-Wno-all', '-fsyntax-only', 'warning.cpp')[2]
        assert (b'warning:' in diagnostic) == (mode == 'gcc'), diagnostic
        invoke(*flags, '-fsyntax-only', 'macro.cpp', status=mode == 'clang')
        invoke(*flags, '-isystem', 'includes', '-fsyntax-only', 'system-macro.cpp')
        invoke(*flags, '-DEXPECTED=8', '-fsyntax-only', 'layout.cpp')
        invoke(*flags, '-fsyntax-only', 'incomplete-self.cpp')
        _, _, diagnostic = invoke(*flags, '-DINCOMPLETE', '-fsyntax-only', 'incomplete-self.cpp', status=1)
        assert b'incomplete' in diagnostic, diagnostic
        invoke(*flags, '-freflection', '-fsyntax-only', 'attribute-splice.cpp')
        invoke(*flags, '-freflection', '-DUNMATCHED_OPENER', '-fsyntax-only', 'attribute-splice.cpp',
               status=mode != 'clang')
        invoke(*flags, '-freflection', '-DUNMATCHED_CLOSER', '-fsyntax-only', 'attribute-splice.cpp', status=1)
        for target in (LINUX, DARWIN):
            invoke(target, *flags, '-fsyntax-only', 'attribute-delimiters.cpp')
            for case in ('BRACE', 'BRACKET', 'PAREN', 'RIGHT_BRACE', 'RIGHT_BRACKET', 'MISSING_OUTER',
                         'UNQUALIFIED', 'ATTR_LIST'):
                invoke(target, *flags, '-D' + case, '-fsyntax-only', 'attribute-delimiters.cpp', status=mode != 'clang')
            for case in ('CROSSED_BRACE', 'CROSSED_BRACKET'):
                invoke(target, *flags, '-D' + case, '-fsyntax-only', 'attribute-delimiters.cpp', status=mode != 'gcc')
            for case in ('RIGHT_PAREN', 'KNOWN_ATTRIBUTE'):
                invoke(target, *flags, '-D' + case, '-fsyntax-only', 'attribute-delimiters.cpp', status=1)
        for target in (LINUX, DARWIN):
            invoke(target, *flags, '-fsyntax-only', 'member-alignment.cpp')
            _, _, diagnostic = invoke(target, *flags, '-fsyntax-only', 'class-head-alignment.cpp')
            assert (b'zero class alignment' in diagnostic) == (mode == 'gcc'), (target, mode, diagnostic)
            _, _, diagnostic = invoke(target, *flags, '-Wno-attributes', '-fsyntax-only', 'class-head-alignment.cpp')
            assert b'warning:' not in diagnostic, (target, mode, diagnostic)
            for case, accepted in CLASS_HEAD_ALIGNMENT_CASES.items():
                _, _, diagnostic = invoke(target, *flags, '-D' + case, '-fsyntax-only',
                                          'class-head-alignment.cpp', status=mode not in accepted)
                if mode not in accepted:
                    assert any(message in diagnostic for message in (
                        b'different alignment requirement', b'requested alignment is weaker',
                        b'definition must specify alignment',
                    )), (target, mode, case, diagnostic)
                if mode == 'gcc' and case in ('LATE_STRONGER', 'ZERO_LATER'):
                    assert b'class alignment is ignored' in diagnostic and b'zero class alignment' not in diagnostic, (
                        target, mode, case, diagnostic)
            layout_flags = [*flags, '-DGCC_MODE'] if mode == 'gcc' else flags
            for width in (1, 2, 4):
                invoke(target, *layout_flags, f'-DWIDTH={width}', '-c', 'clang-under-alignment.cpp',
                       '-o', 'clang-under-alignment.o', status=mode == 'spec')
            for case in ('OBJECT', 'LOCAL', 'MEMBER', 'COPY', 'PTR_ADD', 'BY_VALUE', 'PTR_DIFF'):
                invoke(target, *layout_flags, '-DWIDTH=1', '-D' + case, '-fsyntax-only', 'clang-under-alignment.cpp',
                       status=mode == 'spec')
                invoke(target, *layout_flags, '-DWIDTH=1', '-D' + case, '-c', 'clang-under-alignment.cpp',
                       '-o', 'clang-under-alignment.o', status=mode == 'spec' or (mode == 'clang' and case != 'PTR_DIFF'))
            for case in ('OFFSET', 'CONTAINS'):
                invoke(target, *layout_flags, '-DWIDTH=1', '-D' + case, '-fsyntax-only', 'clang-under-alignment.cpp',
                       status=mode != 'gcc')
        for i, (owner, _) in enumerate(offset_spellings):
            expected = mode == owner
            invoke(*flags, f'-DEXPECTED={int(expected)}', '-fsyntax-only', f'offset-{i}.cpp')
            if not expected:
                _, _, diagnostic = invoke(*flags, '-DEXPECTED=0', '-DFORCE_DIRECTIVE', '-fsyntax-only',
                                         f'offset-{i}.cpp', status=1)
                assert b'unsupported embed parameter' in diagnostic, diagnostic
        invoke(*flags, '-fsyntax-only', 'function-template-pack.cpp', status=mode == 'spec')
        invoke(*flags, '-DVALID_ONLY', '-fsyntax-only', 'function-template-pack.cpp')
        if mode != 'spec':
            invoke(*flags, '-DMISSING', '-fsyntax-only', 'function-template-pack.cpp', status=1)
        invoke(*flags, '-fsyntax-only', 'ref-qualified.cpp', status=mode != 'gcc')
        if mode == 'gcc':
            invoke(*flags, '-DLVALUE_CALL', '-fsyntax-only', 'ref-qualified.cpp', status=1)
            invoke(*flags, '-DSTATIC_MEMBER', '-fsyntax-only', 'ref-qualified.cpp', status=1)
        invoke(*flags, '-fsyntax-only', 'addressof-pack.cpp')
        invoke(*flags, '-DTWO_WRITTEN', '-fsyntax-only', 'addressof-pack.cpp', status=mode == 'gcc')
        invoke(*flags, '-DINSTANTIATE', '-fsyntax-only', 'addressof-pack.cpp', status=mode == 'gcc')
        invoke(*flags, '-fsyntax-only', 'spaceship.cpp')
        invoke(*flags, '-DONLY_UNEQUAL', '-fsyntax-only', 'spaceship.cpp', status=mode == 'gcc')
        invoke(*flags, '-DCONTROL_ONLY', '-fsyntax-only', 'alias-adl.cpp')
        invoke(*flags, '-DFORWARD', '-fsyntax-only', 'alias-adl.cpp', status=mode != 'spec')
        invoke(*flags, '-fsyntax-only', 'alias-adl.cpp', status=mode == 'clang')
        invoke(*flags, '-DORIGINAL', '-DFORWARD', '-fsyntax-only', 'alias-adl.cpp', status=mode != 'gcc')
        if mode != 'spec':
            invoke(*flags, '-DORIGINAL', '-fsyntax-only', 'alias-adl.cpp', status=1)
        invoke(*flags, '-fsyntax-only', 'array-cv.cpp')
        invoke(*flags, '-DMUTATE', '-fsyntax-only', 'array-cv.cpp', status=mode != 'spec')
        invoke(*flags, '-fsyntax-only', 'trait-symbol.cpp')
        _, _, diagnostic = invoke(*flags, '-c', 'trait-symbol.cpp', '-o', 'trait-symbol.o', status=mode == 'gcc')
        if mode == 'gcc':
            assert b'dependent built-in type trait' in diagnostic, diagnostic
        else:
            assert b'_Z9transformIiEu11__remove_cvIT_ES0_\0' in (world.scope / 'trait-symbol.o').read_bytes()
        invoke(*flags, '-DIN_BODY', '-c', 'trait-symbol.cpp', '-o', 'trait-body.o')
        if mode != 'gcc':
            expected_layout = '12' if mode == 'clang' else '8'
            invoke(DARWIN, *flags, '-DEXPECTED=' + expected_layout, '-fsyntax-only', 'layout.cpp')

    assert invoke('-fcompat=gcc', '-fcompat=spec', '-E', 'version.cpp') == default
    for option in ('-fcompat', '-fcompat=', '-fcompat=other'):
        _, _, diagnostic = invoke(option, '-fsyntax-only', 'line.cpp', status=1)
        assert option.encode() in diagnostic, diagnostic
    return pid


def mangling(world):
    scope = world.scope
    (scope / 'symbols.cpp').write_text(MANGLE)
    for mode in MODES:
        output = scope / (mode + '.o')
        cmd = [LINUX, '-fcompat=' + mode, '-c', 'symbols.cpp', '-o', output]
        results = []
        for env in (world.cold, world.daemon):
            result = world.cxx(cmd, scope, env, timeout=40)
            assert result == (0, b'', b''), (mode, result)
            results.append(output.read_bytes())
        assert results[0] == results[1], mode
        names = set(results[0].split(b'\0'))
        for key, alternatives in SYMBOLS.items():
            use_second = mode == ('clang' if key in ('index', 'member') else 'gcc')
            assert alternatives[use_second].encode() in names, (mode, key, sorted(names)[-45:])
            assert alternatives[not use_second].encode() not in names, (mode, key)
        assert b'_Z14different_headITtTyE3TwoEiv' in names, (mode, sorted(names)[-45:])
        assert b'_Z11placeholderITnDaLi4EEiv' in names, (mode, sorted(names)[-45:])
        assert b'_Z8int_headITtTniE2pwEiv' in names, (mode, sorted(names)[-45:])

    source = '''\
int labelled(int) __asm("_first");
int labelled(int) __asm("_last");
int labelled(int v) { return v; }
'''
    (scope / 'asm.cpp').write_text(source)
    for mode in MODES:
        result = world.cxx([LINUX, '-fcompat=' + mode, '-c', 'asm.cpp', '-o', 'asm.o'],
                           scope, world.daemon, timeout=30)
        assert result[0] == (0 if mode == 'gcc' else 1), result
        if mode == 'gcc':
            names = (scope / 'asm.o').read_bytes().split(b'\0')
            assert b'_last' in names and b'_first' not in names, [name for name in names if name.startswith(b'_')]
    (scope / 'asm.cpp').write_text(source.replace('_first', '_next').replace('_last', '_first'))
    command = [LINUX, '-fcompat=gcc', '-c', 'asm.cpp', '-o', 'asm.o']
    for env in (world.daemon, world.cold):
        result = world.cxx(command, scope, env, timeout=30)
        assert result == (0, b'', b''), result
        names = (scope / 'asm.o').read_bytes().split(b'\0')
        assert b'_first' in names and b'_next' not in names, [name for name in names if name.startswith(b'_')]


def mixed_class_calls(world, gxx):
    scope = world.scope
    (scope / 'caller.cpp').write_text(CLASS_DECLARATIONS + CLASS_CALLER)
    (scope / 'peer.cpp').write_text(CLASS_DECLARATIONS + CLASS_PEER)
    cmd = [gxx, '-std=c++26', '-c', 'peer.cpp', '-o', 'peer.o']
    subprocess.run(cmd, cwd=scope, check=True, timeout=40, capture_output=True)
    objects = {}
    for flag in ('-fcompat=gcc', '-fcompat=spec'):
        result = world.cxx([flag, '-c', 'caller.cpp', '-o', 'calls.o'], scope, world.daemon, timeout=40)
        assert result == (0, b'', b''), result
        if flag == '-fcompat=gcc':
            subprocess.run([gxx, 'calls.o', 'peer.o', '-o', 'calls'], cwd=scope, check=True, timeout=40,
                           capture_output=True)
            subprocess.run([scope / 'calls'], cwd=scope, check=True, timeout=20)
        objects[flag] = (scope / 'calls.o').read_bytes()
    assert objects['-fcompat=gcc'] != objects['-fcompat=spec']
    result = world.cxx(['-fcompat=gcc', '-fclass-call-abi=itanium', '-c', 'caller.cpp', '-o', 'calls.o'],
                       scope, world.daemon, timeout=40)
    assert result == (0, b'', b'') and (scope / 'calls.o').read_bytes() == objects['-fcompat=spec'], result


def mixed_darwin_class_calls(world, gxx):
    scope = world.scope
    (scope / 'darwin-caller.cpp').write_text(CLASS_DECLARATIONS + CLASS_CALLER)
    (scope / 'darwin-peer.cpp').write_text(CLASS_DECLARATIONS + CLASS_PEER)
    clang = shutil.which(os.environ.get('YCXX_CLANGXX', 'clang++-23'))
    objects = {}
    for mode in MODES:
        peer = (gxx if mode == 'gcc' else clang) if sys.platform == 'darwin' else None
        command = [DARWIN, '-fcompat=' + mode, '-c', 'darwin-caller.cpp', '-o', 'darwin-caller.o']
        if peer:
            subprocess.run([peer, '-std=c++26', '-c', 'darwin-peer.cpp', '-o', 'darwin-peer.o'],
                           cwd=scope, check=True, capture_output=True, timeout=40)
        outputs = []
        for env in (world.cold, world.daemon):
            result = world.cxx(command, scope, env, timeout=40)
            assert result == (0, b'', b''), (mode, result)
            outputs.append((scope / 'darwin-caller.o').read_bytes())
            if peer:
                subprocess.run([gxx, 'darwin-caller.o', 'darwin-peer.o', '-o', 'darwin-class-calls'],
                               cwd=scope, check=True, capture_output=True, timeout=40)
                checked = subprocess.run([scope / 'darwin-class-calls'], cwd=scope, capture_output=True, timeout=20)
                assert checked.returncode == 0, (mode, peer, env is world.daemon, checked.returncode, checked.stderr)
        assert outputs[0] == outputs[1], mode
        objects[mode] = outputs[0]
    assert objects['spec'] == objects['clang'] and objects['spec'] != objects['gcc']
    for rule, expected in (('itanium', 'spec'), ('apple', 'gcc')):
        command = [DARWIN, '-fcompat=gcc', '-fclass-call-abi=' + rule, '-c', 'darwin-caller.cpp', '-o', 'darwin-caller.o']
        for env in (world.cold, world.daemon):
            result = world.cxx(command, scope, env, timeout=40)
            assert result == (0, b'', b''), (rule, result)
            assert (scope / 'darwin-caller.o').read_bytes() == objects[expected], rule


def mixed_large_class_calls(world, gxx):
    scope = world.scope
    (scope / 'large-caller.cpp').write_text(LARGE_CLASS_DECLARATIONS + LARGE_CLASS_CALLER)
    (scope / 'large-peer.cpp').write_text(LARGE_CLASS_DECLARATIONS + LARGE_CLASS_PEER)
    clang = shutil.which(os.environ.get('YCXX_CLANGXX', 'clang++-23'))
    for mode in MODES:
        peer = gxx if mode == 'gcc' else clang
        if not peer:
            continue
        subprocess.run([peer, '-std=c++26', '-c', 'large-peer.cpp', '-o', 'large-peer.o'],
                       cwd=scope, check=True, capture_output=True, timeout=40)
        command = ['-fcompat=' + mode, '-c', 'large-caller.cpp', '-o', 'large-caller.o']
        for env in (world.cold, world.daemon):
            result = world.cxx(command, scope, env, timeout=40)
            assert result == (0, b'', b''), (mode, result)
            subprocess.run([gxx, 'large-caller.o', 'large-peer.o', '-o', 'large-calls'],
                           cwd=scope, check=True, capture_output=True, timeout=40)
            subprocess.run([scope / 'large-calls'], cwd=scope, check=True, timeout=20)


def mixed_class_alignment(world, gxx):
    scope = world.scope
    header = CLASS_HEAD_ALIGNMENT + 'int read_payload(Payload*);\n'
    (scope / 'alignment-peer.cpp').write_text(header + '''\
int read_payload(Payload* objects) { return sizeof(Payload) * 100 + objects[1].value; }
''')
    (scope / 'alignment-caller.cpp').write_text(header + '''\
int main() {
  Payload objects[2] = {{1}, {17}};
  return read_payload(objects) != sizeof(Payload) * 100 + 17;
}
''')
    clang = shutil.which(os.environ.get('YCXX_CLANGXX', 'clang++-23'))
    for mode in MODES:
        peer = gxx if mode == 'gcc' else clang
        if not peer:
            continue
        definitions = ['-DGCC_MODE'] if mode == 'gcc' else []
        subprocess.run([peer, '-std=c++26', *definitions, '-c', 'alignment-peer.cpp', '-o', 'alignment-peer.o'],
                       cwd=scope, check=True, capture_output=True, timeout=40)
        command = ['-fcompat=' + mode, '-Wno-attributes', *definitions,
                   '-c', 'alignment-caller.cpp', '-o', 'alignment-caller.o']
        for env in (world.cold, world.daemon):
            result = world.cxx(command, scope, env, timeout=40)
            assert result == (0, b'', b''), (mode, result)
            subprocess.run([gxx, 'alignment-caller.o', 'alignment-peer.o', '-o', 'alignment-calls'],
                           cwd=scope, check=True, capture_output=True, timeout=40)
            subprocess.run([scope / 'alignment-calls'], cwd=scope, check=True, timeout=20)


def mixed_constraints(world, gxx):
    scope = world.scope
    definitions = '\n'.join(fn + ' { return 1; }' for fn in CONSTRAINED_FUNCTIONS)
    declarations = '\n'.join(fn + ';' for fn in CONSTRAINED_FUNCTIONS)
    (scope / 'provider.cpp').write_text(CONCEPTS + definitions + '\nint instantiate() { return ' + CONSTRAINED_CALLS + '; }\n')
    (scope / 'consumer.cpp').write_text(
        CONCEPTS + declarations + f'\nint main() {{ return {CONSTRAINED_CALLS} - {len(CONSTRAINED_FUNCTIONS)}; }}\n')
    clang = shutil.which(os.environ.get('YCXX_CLANGXX', 'clang++-23'))
    for mode in MODES:
        command = ['-fcompat=' + mode, '-c', 'provider.cpp', '-o', 'provider.o']
        result = world.cxx(command, scope, world.daemon, timeout=40)
        assert result == (0, b'', b''), (mode, result)
        defined = (scope / 'provider.o').read_bytes()
        symbols = (
            b'_Z10short_formITk6AlwaysiEiT_\0',
            b'_Z4headIiQaa6AlwaysIT_Ent5NeverIS0_EEiS0_\0',
            b'_Z8trailingIiEiT_Qoo6AlwaysIS0_E5NeverIS0_E\0',
            b'_Z8type_reqI6MarkerQrqTNT_4typeEEEiS1_\0',
            b'_Z4bareILb1EEiv\0' if mode == 'gcc' else b'_Z4bareILb1EQT_Eiv\0',
            b'_Z9qualifiedIiQ5CheckIT_cEEiS0_\0' if mode == 'gcc' else
            b'_Z9qualifiedIiQsr1NE5CheckIT_EEiS0_\0',
        )
        for symbol in symbols:
            assert symbol in defined, (mode, symbol, [n for n in defined.split(b'\0') if b'_Z' in n][-30:])
        peer = gxx if mode == 'gcc' else clang
        if peer:
            subprocess.run([peer, '-std=c++26', '-c', 'consumer.cpp', '-o', 'consumer.o'],
                           cwd=scope, check=True, capture_output=True, timeout=40)
            linked = subprocess.run([gxx, 'provider.o', 'consumer.o', '-o', 'constraints'],
                                    cwd=scope, capture_output=True, timeout=40)
            assert linked.returncode == 0, (mode, linked.stderr)
            subprocess.run([scope / 'constraints'], cwd=scope, check=True, timeout=20)


def function_language_linkage(world, gxx):
    scope = world.scope
    (scope / 'language-linkage.cpp').write_text(FUNCTION_LANGUAGE_LINKAGE)
    (scope / 'language-peer.cpp').write_text('''\
extern "C" int named(int);
int declared(int);
namespace N { int declare_in_namespace(int); }
#ifdef SPEC_MODE
#ifdef __APPLE__
#define ABI_NAME "__Z7consumePFYiiE"
#else
#define ABI_NAME "_Z7consumePFYiiE"
#endif
int consume(int (*)(int)) asm(ABI_NAME);
#else
int consume(int (*)(int));
#endif
int main() {
  return consume(&named) != 62 || consume(&declared) != 32 || consume(&N::declare_in_namespace) != 33;
}
''')
    clang = shutil.which(os.environ.get('YCXX_CLANGXX', 'clang++-23'))
    for mode in MODES:
        definitions = ['-DSPEC_MODE'] if mode == 'spec' else []
        flags = ['-fcompat=' + mode, *definitions]
        for target in (LINUX, DARWIN):
            for case, rejected in [('OVERLOAD', mode != 'spec'), ('CONFLICT', True), ('QUALIFIED_CONFLICT', True)]:
                args = [target, *flags, '-D' + case, '-fsyntax-only', 'language-linkage.cpp']
                cold = world.cxx(args, scope, world.cold, timeout=30)
                warm = world.cxx(args, scope, world.daemon, timeout=30)
                assert cold == warm and warm[0] == rejected, (mode, target, case, cold, warm)
            command = [target, *flags, '-c', 'language-linkage.cpp', '-o', 'language.o']
            for env in (world.cold, world.daemon):
                result = world.cxx(command, scope, env, timeout=40)
                assert result == (0, b'', b''), (mode, target, result)
                prefix = b'_' if target == DARWIN else b''
                bare = b'_Z7consumePFYiiE' if mode == 'spec' else b'_Z7consumePFiiE'
                symbols = (scope / 'language.o').read_bytes().split(b'\0')
                assert all(prefix + name in symbols for name in (bare, b'named', b'_Z8declaredi')), (mode, target, symbols)
        peer = gxx if mode in ('spec', 'gcc') else clang
        if peer:
            subprocess.run([peer, '-std=c++26', *definitions, '-c', 'language-peer.cpp', '-o', 'language-peer.o'],
                           cwd=scope, check=True, capture_output=True, timeout=40)
            command = [*flags, '-c', 'language-linkage.cpp', '-o', 'language-native.o']
            result = world.cxx(command, scope, world.daemon, timeout=40)
            assert result == (0, b'', b''), (mode, result)
            subprocess.run([gxx, 'language-native.o', 'language-peer.o', '-o', 'language-calls'],
                           cwd=scope, check=True, capture_output=True, timeout=40)
            subprocess.run([scope / 'language-calls'], cwd=scope, check=True, timeout=20)


def mixed_abstract_result(world, gxx):
    scope = world.scope
    # The ABI symbol for an ordinary function does not encode its result type. A concrete
    # provider with the same result layout can exercise the hidden pointer at this boundary.
    (scope / 'abstract-result-peer.cpp').write_text('''\
int calls;
struct Concrete {
  Concrete() { ++calls; }
  virtual int score(int n) { return n; }
};
Concrete make() { return Concrete{}; }
void discard();
int main() { discard(); return calls != 1; }
''')
    subprocess.run([gxx, '-std=c++26', '-c', 'abstract-result-peer.cpp', '-o', 'abstract-result-peer.o'],
                   cwd=scope, check=True, capture_output=True, timeout=40)
    args = ['-fcompat=clang', '-DCALL', '-c', 'abstract-results.cpp', '-o', 'abstract-result.o']
    for env in (world.cold, world.daemon):
        result = world.cxx(args, scope, env, timeout=40)
        assert result == (0, b'', b''), result
        subprocess.run([gxx, 'abstract-result.o', 'abstract-result-peer.o', '-o', 'abstract-result'],
                       cwd=scope, check=True, capture_output=True, timeout=40)
        subprocess.run([scope / 'abstract-result'], cwd=scope, check=True, timeout=20)


def wide_enumerations(world, gxx):
    scope = world.scope
    (scope / 'wide-enum.cpp').write_text(ENUM_WIDTH)
    clang = shutil.which(os.environ.get('YCXX_CLANGXX', 'clang++-23'))
    for mode in MODES:
        definitions = ['-DCLANG_MODE'] if mode == 'clang' else []
        flags = ['-fcompat=' + mode, *definitions]
        warning = b'incremented enumerator value 18446744073709551616'
        for target in (LINUX, DARWIN):
            for selection in ((), ('-pedantic-errors',), ('-Werror=enum-too-large',),
                              ('-pedantic-errors', '-Wno-enum-too-large'),
                              ('-pedantic-errors', '-Wno-error=enum-too-large')):
                args = [target, *flags, *selection, '-fsyntax-only', 'wide-enum.cpp']
                cold = world.cxx(args, scope, world.cold, timeout=40)
                warm = world.cxx(args, scope, world.daemon, timeout=40)
                rejected = mode == 'clang' and selection in (('-pedantic-errors',), ('-Werror=enum-too-large',))
                assert cold == warm and warm[0] == rejected, (target, mode, selection, cold, warm)
                assert (warning in warm[2]) == (mode == 'clang' and '-Wno-enum-too-large' not in selection), warm
            args = [target, *flags, '-Wno-enum-too-large', '-DPROVIDER', '-c', 'wide-enum.cpp', '-o', 'wide-enum.o']
            for env in (world.cold, world.daemon):
                result = world.cxx(args, scope, env, timeout=40)
                assert result == (0, b'', b''), (target, mode, result)
        peer = clang if mode == 'clang' else gxx
        if peer:
            subprocess.run([peer, '-std=c++26', *definitions, '-DCONSUMER', '-c', 'wide-enum.cpp', '-o', 'enum-peer.o'],
                           cwd=scope, check=True, capture_output=True, timeout=40)
            args = [*flags, '-Wno-enum-too-large', '-DPROVIDER', '-c', 'wide-enum.cpp', '-o', 'enum-native.o']
            for env in (world.cold, world.daemon):
                result = world.cxx(args, scope, env, timeout=40)
                assert result == (0, b'', b''), (mode, result)
                subprocess.run([gxx, 'enum-native.o', 'enum-peer.o', '-o', 'enum-peer'],
                               cwd=scope, check=True, capture_output=True, timeout=40)
                subprocess.run([scope / 'enum-peer'], cwd=scope, check=True, timeout=20)


def wide_array_allocations(world, gxx):
    scope = world.scope
    (scope / 'wide-array-peer.cpp').write_text('''\
using Count = decltype(sizeof(0));
static Count seen;
alignas(long double) static char storage[8];
void* operator new[](Count count) { seen = count; return storage; }
extern "C" Count observed() { return seen; }
''')
    subprocess.run([gxx, '-std=c++26', '-c', 'wide-array-peer.cpp', '-o', 'wide-array-peer.o'],
                   cwd=scope, check=True, capture_output=True, timeout=40)
    for option, count in (('', 1), ('NEGATIVE', 1), ('UNSIGNED_WIDE', 2), ('ZERO', 0)):
        definitions = ['-D' + option] if option else []
        command = ['-fcompat=clang', '-Wno-constant-conversion', *definitions, f'-DEXPECTED={count}', '-c',
                   'wide-array.cpp', '-o', 'wide-array.o']
        for env in (world.cold, world.daemon):
            result = world.cxx(command, scope, env, timeout=40)
            assert result == (0, b'', b''), (option, result)
            subprocess.run([gxx, 'wide-array.o', 'wide-array-peer.o', '-o', 'wide-array'],
                           cwd=scope, check=True, capture_output=True, timeout=40)
            subprocess.run([scope / 'wide-array'], cwd=scope, check=True, timeout=20)


def empty_union(world, gxx):
    scope = world.scope
    (scope / 'empty-union.cpp').write_text(EMPTY_UNION)
    for mode in MODES:
        flags = ['-fcompat=' + mode] + (['-DGCC_MODE'] if mode == 'gcc' else [])
        for target in (LINUX, DARWIN):
            compile_args = [target, *flags, '-c', 'empty-union.cpp', '-o', 'empty-union.o']
            reject_args = [target, *flags, '-DEXPLICIT', '-fsyntax-only', 'empty-union.cpp']
            for env in (world.cold, world.daemon):
                result = world.cxx(compile_args, scope, env, timeout=40)
                assert result == (0, b'', b''), (mode, target, result)
            cold = world.cxx(reject_args, scope, world.cold, timeout=30)
            warm = world.cxx(reject_args, scope, world.daemon, timeout=30)
            assert cold == warm and warm[0] == (mode != 'gcc'), (mode, target, cold, warm)
            for kind in ('ALLOCATE', 'ALLOCATE_CONTROL'):
                args = [target, *flags, '-D' + kind, '-c', 'empty-union.cpp', '-o', 'empty-union-allocation.o']
                cold = world.cxx(args, scope, world.cold, timeout=40)
                warm = world.cxx(args, scope, world.daemon, timeout=40)
                rejected = kind == 'ALLOCATE' and mode == 'gcc'
                assert cold == warm and warm[0] == rejected, (mode, target, kind, cold, warm)
                if rejected:
                    assert warm[2].count(b'deleted') == 2, (mode, target, warm)
        native_cases = [[], ['-DALLOCATE_CONTROL']]
        if mode != 'gcc':
            native_cases.append(['-DALLOCATE'])
        for extra in native_cases:
            command = [*flags, *extra, '-c', 'empty-union.cpp', '-o', 'empty-union-native.o']
            for env in (world.cold, world.daemon):
                result = world.cxx(command, scope, env, timeout=40)
                assert result == (0, b'', b''), (mode, extra, result)
                subprocess.run([gxx, 'empty-union-native.o', '-o', 'empty-union'],
                               cwd=scope, check=True, capture_output=True, timeout=40)
                subprocess.run([scope / 'empty-union'], cwd=scope, check=True, timeout=20)


def pack_index_symbols(world, gxx):
    scope = world.scope
    signatures = [(f'template<class... Ts> int {name}(Ts...[{expr}])', index)
                  for index, (name, expr, _) in enumerate(PACK_INDEX_FUNCTIONS, 1)]
    calls = ' + '.join(f'{name}<int>(0)' for name, _, _ in PACK_INDEX_FUNCTIONS)
    definitions = '\n'.join(f'{signature} {{ return {index}; }}' for signature, index in signatures)
    declarations = '\n'.join(signature + ';' for signature, _ in signatures)
    (scope / 'pack-index-provider.cpp').write_text(
        PACK_INDEX_NAMES + definitions + f'\nint instantiate() {{ return {calls}; }}\n')
    (scope / 'pack-index-consumer.cpp').write_text(
        PACK_INDEX_NAMES + declarations + f'\nint main() {{ return {calls} - {sum(index for _, index in signatures)}; }}\n')
    (scope / 'pack-index-overloads.cpp').write_text(PACK_INDEX_NAMES + '''\
template<class... Ts> int alternate(Ts...[N::zero]) { return 1; }
template<class... Ts> int alternate(Ts...[N::first]) { return 2; }
template<class... Ts> int arithmetic_alternate(Ts...[0]) { return 1; }
template<class... Ts> int arithmetic_alternate(Ts...[0 + 0]) { return 2; }
''')
    clang = shutil.which(os.environ.get('YCXX_CLANGXX', 'clang++-23'))
    for mode in MODES:
        flags = ['-fcompat=' + mode]
        for target in (LINUX, DARWIN):
            args = [target, *flags, '-fsyntax-only', 'pack-index-overloads.cpp']
            cold = world.cxx(args, scope, world.cold, timeout=30)
            warm = world.cxx(args, scope, world.daemon, timeout=30)
            assert cold == warm and warm[0] == (mode == 'spec'), (mode, target, cold, warm)
            command = [target, *flags, '-c', 'pack-index-provider.cpp', '-o', 'pack-index-names.o']
            for env in (world.cold, world.daemon):
                result = world.cxx(command, scope, env, timeout=40)
                assert result == (0, b'', b''), (mode, target, result)
                prefix = '_' if target == DARWIN else ''
                symbols = (scope / 'pack-index-names.o').read_bytes().split(b'\0')
                for name, _, written in PACK_INDEX_FUNCTIONS:
                    encoding = 'Lm0E' if mode == 'spec' else written
                    expected = f'{prefix}_Z{len(name)}{name}IJiEEiDyT_{encoding}'.encode()
                    assert expected in symbols, (mode, target, expected, [s for s in symbols if b'Dy' in s])
        peer = gxx if mode == 'gcc' else clang if mode == 'clang' else None
        if peer:
            subprocess.run([peer, '-std=c++26', '-c', 'pack-index-consumer.cpp', '-o', 'pack-index-consumer.o'],
                           cwd=scope, check=True, capture_output=True, timeout=40)
            command = [*flags, '-c', 'pack-index-provider.cpp', '-o', 'pack-index-native.o']
            result = world.cxx(command, scope, world.daemon, timeout=40)
            assert result == (0, b'', b''), (mode, result)
            subprocess.run([gxx, 'pack-index-native.o', 'pack-index-consumer.o', '-o', 'pack-index-link'],
                           cwd=scope, check=True, capture_output=True, timeout=40)
            subprocess.run([scope / 'pack-index-link'], cwd=scope, check=True, timeout=20)


def bound_pack_index_symbols(world, gxx):
    scope = world.scope
    signatures = (
        'template<class... T> int two(T...[0] a, T...[1] b)',
        'template<class... T> int three(T...[0] a, T...[0] b, T...[0]* p)',
    )
    member = 'template<class... T> struct S { template<int I> T...[I] get()'
    calls = '''\
    S<int, long> s;
    return two<int, char>(4, 5) + three<int>(1, 1, nullptr) + s.get<1>() - 13;
'''
    definitions = (
        signatures[0] + ' { return a + b; }',
        signatures[1] + ' { return a + b + (p ? 1 : 0); }',
        member + ' { return T...[I](I + 1); } };',
        'int instantiate() {' + calls + '}',
    )
    declarations = [signature + ';' for signature in signatures]
    declarations.extend([member + '; };', 'int main() {' + calls + '}'])
    (scope / 'bound-pack-provider.cpp').write_text('\n'.join(definitions))
    (scope / 'bound-pack-consumer.cpp').write_text('\n'.join(declarations))
    clang = shutil.which(os.environ.get('YCXX_CLANGXX', 'clang++-23'))
    for mode in MODES:
        flags = ['-fcompat=' + mode]
        index = 'i' if mode == 'clang' else 'm'
        pack = '_SUBSTPACK_' if mode == 'clang' else 'JilE'
        expected = (
            f'_Z3twoIJicEEiDyT_L{index}0EDyS0_L{index}1E',
            f'_Z5threeIJiEEiDyT_L{index}0ES1_PS1_',
            f'_ZN1SIJilEE3getILi1EEEDy{pack}T_v',
        )
        for target in (LINUX, DARWIN):
            command = [target, *flags, '-c', 'bound-pack-provider.cpp', '-o', 'bound-pack.o']
            for env in (world.cold, world.daemon):
                result = world.cxx(command, scope, env, timeout=40)
                assert result == (0, b'', b''), (mode, target, result)
                prefix = '_' if target == DARWIN else ''
                symbols = (scope / 'bound-pack.o').read_bytes().split(b'\0')
                for symbol in expected:
                    assert (prefix + symbol).encode() in symbols, (mode, target, symbol, [s for s in symbols if b'Dy' in s])
        peer = clang if mode == 'clang' else gxx
        if peer:
            subprocess.run([peer, '-std=c++26', '-c', 'bound-pack-consumer.cpp', '-o', 'bound-pack-consumer.o'],
                           cwd=scope, check=True, capture_output=True, timeout=40)
            command = [*flags, '-c', 'bound-pack-provider.cpp', '-o', 'bound-pack-native.o']
            result = world.cxx(command, scope, world.daemon, timeout=40)
            assert result == (0, b'', b''), (mode, result)
            subprocess.run([gxx, 'bound-pack-native.o', 'bound-pack-consumer.o', '-o', 'bound-pack-link'],
                           cwd=scope, check=True, capture_output=True, timeout=40)
            subprocess.run([scope / 'bound-pack-link'], cwd=scope, check=True, timeout=20)


def operator_constraints(world, gxx):
    scope = world.scope
    declarations = '\n'.join(fn + ';' for fn, _, _ in OPERATOR_CONSTRAINTS)
    definitions = '\n'.join(fn + ' { return 1; }' for fn, _, _ in OPERATOR_CONSTRAINTS)
    calls = ' + '.join(call for _, call, _ in OPERATOR_CONSTRAINTS)
    symbols = [symbol.encode() for _, _, symbol in OPERATOR_CONSTRAINTS]
    (scope / 'operator-constraints.cpp').write_text(definitions + f'\nint instantiate() {{ return {calls}; }}\n')
    (scope / 'operator-consumer.cpp').write_text(
        declarations + f'\nint main() {{ return {calls} - {len(OPERATOR_CONSTRAINTS)}; }}\n')
    clang = shutil.which(os.environ.get('YCXX_CLANGXX', 'clang++-23'))
    for mode in MODES:
        args = ['-fcompat=' + mode, '-c', 'operator-constraints.cpp', '-o', 'operator-constraints.o']
        for target in (LINUX, DARWIN):
            prefix = b'_' if target == DARWIN else b''
            for env in (world.cold, world.daemon):
                result = world.cxx([target, *args], scope, env, timeout=40)
                assert result == (0, b'', b''), (mode, target, result)
                names = (scope / 'operator-constraints.o').read_bytes().split(b'\0')
                for symbol in symbols:
                    assert prefix + symbol in names, (mode, target, symbol, [n for n in names if b'_Z' in n][-40:])
        peer = gxx if mode == 'gcc' else clang
        if peer:
            result = world.cxx(args, scope, world.daemon, timeout=40)
            assert result == (0, b'', b''), (mode, result)
            subprocess.run([peer, '-std=c++26', '-c', 'operator-consumer.cpp', '-o', 'operator-consumer.o'],
                           cwd=scope, check=True, capture_output=True, timeout=40)
            subprocess.run([gxx, 'operator-constraints.o', 'operator-consumer.o', '-o', 'operator-link'],
                           cwd=scope, check=True, capture_output=True, timeout=40)
            subprocess.run([scope / 'operator-link'], cwd=scope, check=True, timeout=20)


def requires_expression_symbols(world, gxx):
    scope = world.scope
    preamble = '''\
template<class T> concept Any = true;
namespace N { template<class T, class U = int> concept Check = true; }
struct Item { using type = int; };
'''
    cases = (
        ('template<class T> requires requires (T t) { t + t; } int simple(T)', 'simple(1)'),
        ('template<class T> requires requires (T t) { {t + t} noexcept; } int non_throwing(T)', 'non_throwing(1)'),
        ('template<class T> requires requires (T t) { {t + t} -> Any; } int returns_any(T)', 'returns_any(1)'),
        ('template<class T> requires requires { typename T::type; typename T::type; } int twice(T)', 'twice(Item{})'),
        ('template<class T> int empty(T) requires requires () { sizeof(T); }', 'empty(1)'),
        ('template<class T> int unnamed(T) requires requires (T, T b) { b; }', 'unnamed(1)'),
        ('template<class T> int adjusted(T) requires requires (T a[3]) { *a; }', 'adjusted(1)'),
        ('template<class T> int atoms(T) requires requires { requires sizeof(T) > 1 && sizeof(T) < 8; }', 'atoms(1)'),
        ('template<class T> int inner(T) requires requires (T a) { requires requires (T b) { a + b; }; }', 'inner(1)'),
        ('template<class T> int cv_param(T) requires requires (const T a, T b) { a + b; }', 'cv_param(1)'),
        ('template<class T> int result(T) requires requires (T a) { {a} -> N::Check<T>; }', 'result(1)'),
        ('template<class T> int result_default(T) requires requires (T a) { {a} -> N::Check; }', 'result_default(1)'),
    )
    calls = ' + '.join(call for _, call in cases)
    definitions = '\n'.join(signature + ' { return 1; }' for signature, _ in cases)
    declarations = '\n'.join(signature + ';' for signature, _ in cases)
    (scope / 'requires-provider.cpp').write_text(preamble + definitions + f'\nint instantiate() {{ return {calls}; }}\n')
    (scope / 'requires-consumer.cpp').write_text(
        preamble + declarations + f'\nint main() {{ return {calls} - {len(cases)}; }}\n')
    clang = shutil.which(os.environ.get('YCXX_CLANGXX', 'clang++-23'))
    for mode in MODES:
        param = 'fL0p_' if mode == 'clang' else 'fp_'
        empty = 'rq' if mode == 'gcc' else 'rQv_'
        nested = {'spec': 'fL0p_fp_', 'gcc': 'fp_fL18446744073709551614p_', 'clang': 'fL0p_fL0p_'}[mode]
        qualifier = '' if mode == 'gcc' else 'K'
        result = 'N1N5CheckEIS0_E' if mode == 'gcc' else 'N1N5CheckIS0_EE'
        default = 'IiE' if mode == 'gcc' else ''
        symbols = (
            f'_Z6simpleIiQrQT__Xpl{param}{param}EEiS0_',
            f'_Z12non_throwingIiQrQT__Xpl{param}{param}NEEiS0_',
            f'_Z11returns_anyIiQrQT__Xpl{param}{param}R3AnyEEiS0_',
            '_Z5twiceI4ItemQrqTNT_4typeETS2_EEiS1_',
            f'_Z5emptyIiEiT_Q{empty}XstS0_E',
            '_Z7unnamedIiEiT_QrQS0_S0__Xfp0_E',
            '_Z8adjustedIiEiT_QrQPS0__Xdefp_E',
            '_Z5atomsIiEiT_QrqQaagtstS0_Li1EltstS0_Li8EE',
            f'_Z5innerIiEiT_QrQS0__QrQS0__Xpl{nested}EE',
            f'_Z8cv_paramIiEiT_QrQS0_S0__Xplfp{qualifier}_fp0_E',
            f'_Z6resultIiEiT_QrQS0__Xfp_R{result}E',
            f'_Z14result_defaultIiEiT_QrQS0__Xfp_RN1N5CheckE{default}E',
        )
        command = ['-fcompat=' + mode, '-c', 'requires-provider.cpp', '-o', 'requires-provider.o']
        for target in (LINUX, DARWIN):
            prefix = '_' if target == DARWIN else ''
            for env in (world.cold, world.daemon):
                outcome = world.cxx([target, *command], scope, env, timeout=40)
                assert outcome == (0, b'', b''), (mode, target, outcome)
                names = (scope / 'requires-provider.o').read_bytes().split(b'\0')
                for symbol in symbols:
                    assert (prefix + symbol).encode() in names, (mode, target, symbol, [n for n in names if b'_Z' in n])
        peer = gxx if mode == 'gcc' else clang if mode == 'clang' else None
        if peer:
            outcome = world.cxx(command, scope, world.daemon, timeout=40)
            assert outcome == (0, b'', b''), (mode, outcome)
            subprocess.run([peer, '-std=c++26', '-c', 'requires-consumer.cpp', '-o', 'requires-consumer.o'],
                           cwd=scope, check=True, capture_output=True, timeout=40)
            subprocess.run([gxx, 'requires-provider.o', 'requires-consumer.o', '-o', 'requires-link'],
                           cwd=scope, check=True, capture_output=True, timeout=40)
            subprocess.run([scope / 'requires-link'], cwd=scope, check=True, timeout=20)


def template_argument_prefixes(world, gxx):
    scope = world.scope
    functions = [fn for fn, _, _, _ in PREFIX_FUNCTIONS if fn]
    calls = ' + '.join(call for _, call, _, _ in PREFIX_FUNCTIONS)
    definitions = '\n'.join(fn + ' { return 1; }' for fn in functions)
    declarations = '\n'.join(fn + ';' for fn in functions)
    (scope / 'prefix-provider.cpp').write_text(PREFIX_TEMPLATES + definitions + f'\nint instantiate() {{ return {calls}; }}\n')
    (scope / 'prefix-consumer.cpp').write_text(
        PREFIX_TEMPLATES + declarations + f'\nint main() {{ return {calls} - {len(PREFIX_FUNCTIONS)}; }}\n')
    clang = shutil.which(os.environ.get('YCXX_CLANGXX', 'clang++-23'))
    for mode in MODES:
        symbols = [row[2 if mode == 'gcc' else 3].encode() for row in PREFIX_FUNCTIONS]
        command = ['-fcompat=' + mode, '-c', 'prefix-provider.cpp', '-o', 'prefix-provider.o']
        for target in (LINUX, DARWIN):
            prefix = b'_' if target == DARWIN else b''
            for env in (world.cold, world.daemon):
                result = world.cxx([target, *command], scope, env, timeout=40)
                assert result == (0, b'', b''), (mode, target, result)
                names = (scope / 'prefix-provider.o').read_bytes().split(b'\0')
                assert all(prefix + symbol in names for symbol in symbols), (mode, target, symbols)
        peer = gxx if mode == 'gcc' else clang
        if peer:
            result = world.cxx(command, scope, world.daemon, timeout=40)
            assert result == (0, b'', b''), (mode, result)
            subprocess.run([peer, '-std=c++26', '-c', 'prefix-consumer.cpp', '-o', 'prefix-consumer.o'],
                           cwd=scope, check=True, capture_output=True, timeout=40)
            subprocess.run([gxx, 'prefix-provider.o', 'prefix-consumer.o', '-o', 'prefix-link'],
                           cwd=scope, check=True, capture_output=True, timeout=40)
            subprocess.run([scope / 'prefix-link'], cwd=scope, check=True, timeout=20)


def mixed_source_location(world, gxx):
    scope = world.scope
    (scope / 'descriptor-peer.cpp').write_text('''\
#include <source_location>
extern "C" const std::source_location recorded_location;
int main() {
#ifdef GCC_MODE
  return recorded_location.line() != 101 || recorded_location.column() != 3;
#else
  return recorded_location.line() != 100 || recorded_location.column() != 27;
#endif
}
''')
    libycxx = Path(__file__).resolve().parents[2] / 'deps' / 'libycxx'
    includes = libycxx / 'include'
    marker = libycxx / 'src' / 'runtime' / 'linkage' / 'static.cpp'
    subprocess.run([gxx, '-std=c++26', '-isystem', str(includes), '-c', str(marker), '-o', 'descriptor-runtime.o'],
                   cwd=scope, check=True, capture_output=True, timeout=40)
    clang = shutil.which(os.environ.get('YCXX_CLANGXX', 'clang++-23'))
    for mode in MODES:
        peer = gxx if mode == 'gcc' else clang or gxx
        definitions = ['-DGCC_MODE'] if mode == 'gcc' else []
        subprocess.run([peer, '-std=c++26', '-isystem', str(includes), *definitions, '-c',
                        'descriptor-peer.cpp', '-o', 'descriptor-peer.o'],
                       cwd=scope, check=True, capture_output=True, timeout=40)
        command = ['-fcompat=' + mode, *definitions, '-c', 'descriptor.cpp', '-o', 'descriptor-native.o']
        for env in (world.cold, world.daemon):
            result = world.cxx(command, scope, env, timeout=40)
            assert result == (0, b'', b''), (mode, result)
            linked = subprocess.run([gxx, 'descriptor-native.o', 'descriptor-peer.o', 'descriptor-runtime.o',
                                     '-o', 'source-location'], cwd=scope, capture_output=True, timeout=40)
            assert linked.returncode == 0, (mode, linked.stderr)
            subprocess.run([scope / 'source-location'], cwd=scope, check=True, timeout=20)


def mixed_tail_padding(world, gxx):
    scope = world.scope
    (scope / 'tail-reader.cpp').write_text(TAIL_PADDING + '''\
extern "C" int read_tail(const Derived& object) { return object.tail; }
''')
    (scope / 'tail-peer.cpp').write_text(TAIL_PADDING + '''\
extern "C" int read_tail(const Derived&);
int main() {
  Derived object;
  object.tail = 17;
  return read_tail(object) != 17;
}
''')
    clang = shutil.which(os.environ.get('YCXX_CLANGXX', 'clang++-23'))
    for mode in MODES:
        for feature in ('PRIVATE_MEMBERS', 'OVERLAPPING_MEMBER'):
            peer = clang if mode == 'clang' or (mode == 'spec' and feature == 'OVERLAPPING_MEMBER') else gxx
            if not peer:
                continue
            definition = '-D' + feature
            subprocess.run([peer, '-std=c++26', definition, '-c', 'tail-peer.cpp', '-o', 'tail-peer.o'],
                           cwd=scope, check=True, capture_output=True, timeout=40)
            command = ['-fcompat=' + mode, definition, '-c', 'tail-reader.cpp', '-o', 'tail-reader.o']
            for env in (world.cold, world.daemon):
                result = world.cxx(command, scope, env, timeout=40)
                assert result == (0, b'', b''), (mode, feature, result)
                subprocess.run([gxx, 'tail-reader.o', 'tail-peer.o', '-o', 'tail-calls'],
                               cwd=scope, check=True, capture_output=True, timeout=40)
                subprocess.run([scope / 'tail-calls'], cwd=scope, check=True, timeout=20)


def compile_database(world):
    scope = world.scope
    unit = scope / 'editor.cpp'
    text = 'static_assert(__is_same(decltype(__builtin_LINE()), int));\n'
    unit.write_text(text)
    editor = Editor(world, scope)
    for version, mode in enumerate(('spec', 'gcc', 'clang', 'gcc'), 1):
        (scope / 'compile_commands.json').write_text(json.dumps([{
            'directory': str(scope), 'file': unit.name,
            'arguments': ['c++', LINUX, '-fcompat=' + mode, '-c', unit.name, '-o', 'editor.o'],
        }]))
        if version == 1:
            editor.open(unit, text)
        else:
            editor.change(unit, text, version)
        context = f'compile database with {mode} compatibility'
        found = editor.diagnostics(unit, version, context)
        if mode == 'gcc':
            expect(found, [], context)
        else:
            assert found and any(line == 0 and severity == 1 and 'static assertion' in message
                                 for line, _, severity, message in found), (context, found)
    expect(editor.finish(), 0, 'compatibility LSP shutdown')


def main():
    ycxx, gxx = sys.argv[1:]
    world = Env(str(Path(ycxx).resolve()))
    for env in (world.base, world.cold, world.daemon):
        env.update(LC_ALL='C', YCXX_THREADS='2')
    try:
        pid = preprocess_and_syntax(world)
        mangling(world)
        # PARITY: by design: the parameter difference here is the SysV AMD64 MEMORY class.
        if NATIVE_LINUX:
            mixed_class_calls(world, gxx)
            mixed_large_class_calls(world, gxx)
            # PARITY: by design: native array new is currently available on Linux x86-64.
            wide_array_allocations(world, gxx)
        mixed_darwin_class_calls(world, gxx)
        mixed_class_alignment(world, gxx)
        mixed_tail_padding(world, gxx)
        mixed_constraints(world, gxx)
        function_language_linkage(world, gxx)
        mixed_abstract_result(world, gxx)
        wide_enumerations(world, gxx)
        empty_union(world, gxx)
        pack_index_symbols(world, gxx)
        bound_pack_index_symbols(world, gxx)
        operator_constraints(world, gxx)
        requires_expression_symbols(world, gxx)
        template_argument_prefixes(world, gxx)
        mixed_source_location(world, gxx)
        compile_database(world)
        world.same_daemon('compatibility modes', pid)
    finally:
        world.close()
    return bool(failures)


if __name__ == '__main__':
    sys.exit(main())
