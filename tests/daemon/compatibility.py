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
#elif defined(PROTECTED_MEMBERS)
struct Base { protected: int value; char byte; };
#elif defined(MIXED_ACCESS)
struct Base { int value; private: char byte; };
#elif defined(DEFAULT_MEMBER_INITIALIZER)
struct Base { int value = 0; char byte; };
#elif defined(FIRST_DEFAULT_CONSTRUCTOR)
struct Base { int value; char byte; Base() = default; };
#elif defined(LATE_DEFAULT_CONSTRUCTOR)
struct Base { int value; char byte; Base(); };
Base::Base() = default;
#elif defined(LATE_DEFAULT_ASSIGNMENT)
struct Base { int value; char byte; Base& operator=(const Base&); };
Base& Base::operator=(const Base&) = default;
#elif defined(LATE_DEFAULT_DESTRUCTOR)
struct Base { int value; char byte; ~Base(); };
Base::~Base() = default;
#elif defined(CONST_MEMBER)
struct Base { const int value; char byte; };
#elif defined(REFERENCE_MEMBER)
struct Base { int& value; char byte; };
#elif defined(ARRAY_MEMBER)
struct Private { private: int value; char byte; };
struct Base { Private array[2]; char byte; };
#elif defined(EMPTY_BASE)
struct Empty {};
struct Base : Empty { int value; char byte; };
#elif defined(NESTED_OVERLAPPING_MEMBER)
struct Plain { int value; char byte; };
struct Base { [[no_unique_address]] Plain member; char byte; };
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
template<class, class = int> struct Two {};
template<template<class> class T> int different_head() { return 5; }
template<class T> struct Y { template<class> using apply = T; };
template<template<class> class Q> struct Use {};
template<class T> int member(Use<Y<T>::template apply>) { return 3; }
int use() {
  return variable<int>(Box<true>{0}) + index<int>(0) + typed<int, 4>()
       + member<int>(Use<Y<int>::apply>{}) + empty_values<int>() + different_head<Two>();
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
    'member': ('_Z6memberIiEi3UseIN1YIT_E5applyEE', '_Z6memberIiEi3UseIsr1YIT_EE5applyE'),
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
    (world.scope / 'memory-long-double.cpp').write_text('''\
constexpr long double one = 1.0L;
#ifdef X87
constexpr long double slight = 1.0000000000000000001084202172485504434007L;
constexpr long double midpoint = 1.0000000000000000000542101086242752217003726400434970855712890625L;
constexpr long double full = 0x1.fffffffffffffffep-1000L;
constexpr long double huge = 1.0e4000L;
constexpr long double zero = 0.0L, least = 0x1p-16445L;
constexpr long double half_least = 0x1p-16446L;
constexpr long double above_half_least = 0x1.0000000000000002p-16446L;
constexpr long double first_normal = 0x1p-16382L;
constexpr long double tie_to_normal = 0x0.ffffffffffffffffp-16382L;
static_assert(sizeof(one) == 16 && sizeof(huge) == 16);
#ifndef CONTROL
static_assert(__builtin_memcmp(&one, &slight, sizeof one) < 0);
static_assert(__builtin_memcmp(&one, &midpoint, sizeof one) == 0);
static_assert(__builtin_memcmp(&zero, &half_least, sizeof zero) == 0);
static_assert(__builtin_memcmp(&least, &above_half_least, sizeof least) == 0);
static_assert(__builtin_memcmp(&first_normal, &tie_to_normal, sizeof first_normal) == 0);
static_assert(__builtin_memchr(&one, 0xff, 10) != nullptr);
static_assert(__builtin_memchr(&full, 0, 10) == nullptr);
static_assert(__builtin_memchr(&full, 0, sizeof full) != nullptr);
#endif
#else
constexpr long double two = 2.0L;
static_assert(sizeof(one) == 8);
#ifndef CONTROL
static_assert(__builtin_memcmp(&one, &two, sizeof one) > 0);
static_assert(__builtin_memchr(&one, 0x3f, sizeof one) != nullptr);
#endif
#endif
''')
    (world.scope / 'memory-union-object.cpp').write_text('''\
union Sample { unsigned char first; unsigned long long second; };
constexpr Sample value{1}, larger{2}, empty{};
#if defined(INACTIVE)
static_assert(empty.second == 0);
#elif defined(LOCAL)
constexpr bool test() {
  Sample local{1};
  return __builtin_memchr(&local, 1, sizeof local) == &local;
}
static_assert(test());
#elif defined(BYTES)
static_assert(__builtin_memcmp(&value, &larger, sizeof value) < 0);
static_assert(__builtin_memchr(&value, 1, sizeof value) == &value.first);
static_assert(__builtin_memchr(&value, 0, 1) == nullptr);
static_assert(__builtin_memchr(&value, 0, sizeof value) != nullptr);
#else
static_assert(value.first == 1 && empty.first == 0);
constexpr bool test() { Sample local{5}; return local.first == 5; }
static_assert(test());
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
    (world.scope / 'memory-nested-record.cpp').write_text('''\
struct Inner { char tag; unsigned word; };
struct Outer { char head; Inner inner[2]; unsigned tail; };
constexpr Outer left{13, {{15, 17}, {19, 21}}, 25};
constexpr Outer right{13, {{15, 17}, {19, 21}}, 26};
static_assert(left.inner[1].word == 21 && right.tail == 26);
#ifdef BYTE_READ
static_assert(__builtin_memcmp(&left, &right, sizeof left) < 0);
static_assert(__builtin_memcmp(&left.inner, &right.inner, sizeof left.inner) == 0);
static_assert(__builtin_memchr(&left, 21, sizeof left) == &left.inner[1].word);
static_assert(__builtin_memchr(&left, 0, sizeof left) != nullptr);
#endif
''')
    (world.scope / 'constexpr-empty-union.cpp').write_text('''\
struct Variant { unsigned word; constexpr Variant() : word(17) {} };
union U { Variant first; unsigned other; };
constexpr U value{};
static_assert(value.first.word == EXPECTED);
''')
    (world.scope / 'constexpr-union-destruction.cpp').write_text('''\
void runtime_only();
struct Variant {
  unsigned word;
  mutable unsigned scratch;
  constexpr Variant() : word(17), scratch(3) {}
  constexpr ~Variant() {}
};
union U {
  Variant first;
  unsigned later;
#if defined(NON_CONSTEXPR)
  ~U() {}
#elif defined(CONDITIONAL)
  constexpr ~U() { if (first.word == 17) runtime_only(); }
#elif defined(BAD)
  constexpr ~U() { if (first.word == 17 || first.word == 0) runtime_only(); }
#elif defined(MUTABLE_READ)
  constexpr ~U() { if (first.scratch != 3) {} }
#elif defined(MUTABLE_WRITE)
  constexpr ~U() { ++first.scratch; }
#elif defined(WRITE)
  constexpr ~U() { ++first.word; }
#else
  constexpr ~U() {}
#endif
};
#ifdef LOCAL
constexpr unsigned read() { constexpr U value{}; return value.first.word; }
static_assert(read() == EXPECTED);
#else
constexpr U value{};
static_assert(value.first.word == EXPECTED);
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
    ctz_fixtures = Path(__file__).with_name('builtin_ctz')
    ctz_cases = ('zero-literal.cpp', 'zero-parameter.cpp')
    for case in ctz_cases:
        shutil.copyfile(ctz_fixtures / case, world.scope / case)
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
                'PROTECTED_MEMBERS': 12 if darwin_clang else 8,
                'MIXED_ACCESS': 8,
                'DEFAULT_MEMBER_INITIALIZER': 8,
                'FIRST_DEFAULT_CONSTRUCTOR': 12,
                'LATE_DEFAULT_CONSTRUCTOR': 8,
                'LATE_DEFAULT_ASSIGNMENT': 8,
                'LATE_DEFAULT_DESTRUCTOR': 8,
                'CONST_MEMBER': 12,
                'REFERENCE_MEMBER': 16,
                'ARRAY_MEMBER': 24 if darwin_clang else 20,
                'EMPTY_BASE': 12 if darwin_clang else 8,
                'NESTED_OVERLAPPING_MEMBER': 12 if mode == 'gcc' else 16,
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
            for case in ctz_cases:
                diagnostic = invoke(target, *flags, '-fsyntax-only', case, status=mode != 'gcc')[2]
                if mode != 'gcc':
                    assert b'counting trailing zeros with a zero operand' in diagnostic, diagnostic
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
            long_double = ['-DX87'] if target == LINUX else []
            invoke(target, *flags, *long_double, '-DCONTROL', '-fsyntax-only', 'memory-long-double.cpp')
            invoke(target, *flags, *long_double, '-fsyntax-only', 'memory-long-double.cpp', status=mode != 'gcc')
            invoke(target, *flags, '-fsyntax-only', 'memory-union-object.cpp')
            invoke(target, *flags, '-DBYTES', '-fsyntax-only', 'memory-union-object.cpp', status=mode != 'gcc')
            for case in ('LOCAL', 'INACTIVE'):
                invoke(target, *flags, '-D' + case, '-fsyntax-only', 'memory-union-object.cpp', status=1)
            for case in ('', 'ARRAY', 'REUSE', 'LOCAL'):
                args = ['-D' + case] if case else []
                invoke(target, *flags, *args, '-fsyntax-only', 'memory-record-object.cpp',
                       status=mode != 'gcc' or case in ('REUSE', 'LOCAL'))
            invoke(target, *flags, '-fsyntax-only', 'memory-nested-record.cpp')
            invoke(target, *flags, '-DBYTE_READ', '-fsyntax-only', 'memory-nested-record.cpp', status=mode != 'gcc')
            union_expected = 0 if mode == 'gcc' else 17
            invoke(target, *flags, f'-DEXPECTED={union_expected}', '-fsyntax-only', 'constexpr-empty-union.cpp')
            cases = {'': 0, 'WRITE': 0, 'NON_CONSTEXPR': 1, 'BAD': 1,
                     'CONDITIONAL': mode != 'gcc', 'MUTABLE_READ': mode != 'gcc', 'MUTABLE_WRITE': mode != 'gcc'}
            for case, status in cases.items():
                definition = [f'-D{case}'] if case else []
                for scope in ([], ['-DLOCAL']):
                    invoke(target, *flags, f'-DEXPECTED={union_expected}', *definition, *scope,
                           '-fsyntax-only', 'constexpr-union-destruction.cpp', status=status)
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
        closure = {'spec': 'UliE_8__invoke', 'gcc': 'UliE_4_FUN', 'clang': '3$_08__invoke'}
        for candidate, component in closure.items():
            assert (f'_ZZ7closureiEN{component}Ei'.encode() in names) == (mode == candidate), (mode, names)
        assert b'_Z14different_headITtTyE3TwoEiv' in names, (mode, sorted(names)[-45:])

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


def private_closure_symbols(world, gxx):
    scope = world.scope
    (scope / 'private-closures.cpp').write_text('''\
using F = int(*)(int);
using L = int(*)(long);
F direct(int flag) {
  auto unused = [](int n) { return n; };
  auto a = [](int n) { return n + 1; };
  auto b = [](long n) { return int(n) + 2; };
  auto c = [](int n) { return n + 3; };
  int v = a(flag);
  L fn = b;
  if (flag && v == 1 && fn(0) == 2) return c;
  return a;
}
static F internal(int flag);
F internal(int flag) {
  auto unused = [](int n) { return n; };
  auto a = [](int n) { return n + 1; };
  auto b = [](long n) { return int(n) + 2; };
  auto c = [](int n) { return n + 3; };
  L fn = b;
  if (flag && fn(0) == 2) return a;
  return c;
}
template<class T> int consume(T value) { return value(1); }
int needs_type() {
  auto a = [](int n) { return n; };
  auto b = [](int n) { return n+2; };
  F f = b;
  return f(2) + consume(a);
}
static inline F file_inline() { return [](int v) { return v + 4; }; }
template<class T> static F file_template(T) { return [](int v) { return v + 5; }; }
struct Out { static F member(); static F inside() { return [](int v) { return v + 6; }; } };
F Out::member() { return [](int v) { return v + 7; }; }
extern "C" F c_owner() { return [](int v) { return v + 8; }; }
extern "C" int exercise() {
  return direct(0)(0) + internal(0)(0) + needs_type() + file_inline()(0) + file_template(0)(0)
      + Out::inside()(0) + Out::member()(0) + c_owner()(0);
}
''')
    (scope / 'private-closure-caller.cpp').write_text('extern "C" int exercise();\n'
                                                     'int main() { return exercise() != 39; }\n')
    clang = shutil.which(os.environ.get('YCXX_CLANGXX', 'clang++-23'))
    for mode in MODES:
        invoker = '4_FUN' if mode == 'gcc' else '8__invoke'
        local = 'L' if mode != 'spec' else ''
        direct = ('3$_0', '3$_1', '3$_2') if mode == 'clang' else ('UliE0_', 'UllE_', 'UliE1_')
        internal = ('3$_1', '3$_0', '3$_2') if mode == 'clang' else direct
        expected = []
        for owner, components in (('6directi', direct), (local + '8internali', internal)):
            for component, parameter in zip(components, 'ili'):
                expected += [f'_ZZ{owner}EN{component}{invoker}E{parameter}', f'_ZZ{owner}ENK{component}clE{parameter}']
        expected += [
            f'_Z7consumeIZ10needs_typevE{"3$_1" if mode == "clang" else "UliE_"}EiT_',
            f'_ZZ{local}11file_inlinevENUliE_{invoker}Ei',
            f'_ZZ{"L" if mode == "clang" else ""}13file_templateIiEPFiiET_ENUliE_{invoker}Ei',
            f'_ZZN3Out6insideEvENUliE_{invoker}Ei',
            f'_ZZN3Out6memberEvEN{"3$_0" if mode == "clang" else "UliE_"}{invoker}Ei',
            f'_ZZ{"7c_ownerv" if mode == "spec" else "7c_owner"}EN{"3$_0" if mode == "clang" else "UliE_"}{invoker}Ei',
        ]
        command = ['-fcompat=' + mode, '-c', 'private-closures.cpp', '-o', 'private-closures.o']
        for target in (LINUX, DARWIN):
            prefix = '_' if target == DARWIN else ''
            binaries = []
            for env in (world.cold, world.daemon):
                result = world.cxx([target, *command], scope, env, timeout=40)
                assert result == (0, b'', b''), (mode, target, result)
                binaries.append((scope / 'private-closures.o').read_bytes())
            assert binaries[0] == binaries[1], (mode, target)
            names = set(binaries[0].split(b'\0'))
            for symbol in expected:
                assert (prefix + symbol).encode() in names, (mode, target, symbol, sorted(n for n in names if b'_ZZ' in n))
            if mode != 'spec':
                for owner in ('6directi', local + '8internali'):
                    unused = '3$_3' if mode == 'clang' else 'UliE_'
                    assert (prefix + f'_ZZ{owner}ENK{unused}clEi').encode() not in names, (mode, target, names)
            if (target == LINUX and NATIVE_LINUX) or (target == DARWIN and sys.platform == 'darwin'):
                peer = clang if mode == 'clang' else gxx
                if peer:
                    subprocess.run([peer, '-std=c++26', 'private-closure-caller.cpp', 'private-closures.o',
                                    '-o', 'private-closures'], cwd=scope, check=True, capture_output=True, timeout=40)
                    subprocess.run([scope / 'private-closures'], cwd=scope, check=True, timeout=20)


def generic_invoker_symbols(world, gxx):
    scope = world.scope
    (scope / 'generic-invoker.cpp').write_text('''\
using I = int(*)(int);
using R = int(*)(const int&);
using B = int(*)(int, int);
using M = int(*)(int, long);
using N = int(*)(int) noexcept;
using V = void(*)(int);
inline I typed() { return []<class T>(T v) -> T { return v; }; }
inline I fixed() { return [](auto v) -> int { return v; }; }
inline R lref() { return [](const auto& v) { return v; }; }
inline B same() { return []<class T>(T a, T b) { return a + b; }; }
inline M multi() { return []<class T, class U>(T a, U b) { return a+int(b); }; }
inline I mut() { return [](auto v) mutable { return v; }; }
inline N noex() { return [](auto v) noexcept { return v; }; }
inline I drop_noex() { return [](auto v) noexcept { return v; }; }
inline V vvoid() { return [](auto v) -> void { (void)v; }; }
inline M packed() { return []<class... T>(T... v) { return static_cast<int>((v + ...)); }; }
inline I defaulted() { return []<class T, int N=7>(T v) { return v + N; }; }
inline I inferred() { return [](auto v) { return v; }; }
I private_generic() {
  auto unused = [](auto v) { return v; };
  return [](auto v) { return v + 1; };
}
extern "C" int exercise() {
  int val = 2;
  vvoid()(0);
  return typed()(1) + fixed()(1) + lref()(val) + same()(1, 2) + multi()(1, 2L)
      + mut()(1) + noex()(1) + drop_noex()(1) + packed()(1, 2L) + defaulted()(1) + inferred()(1);
}
''')
    (scope / 'generic-invoker-constant.cpp').write_text('''\
constexpr auto lim = []<class T>(T n) { return n + 2; };
using Fn = int(*)(int);
#if defined(CAPTURE)
Fn bad() { return [n = 2](auto v) { return v + n; }; }
#elif defined(RETURN_TYPE)
Fn bad() { return [](auto v) -> long { return v; }; }
#elif defined(NOEXCEPT)
using Safe = int(*)(int) noexcept;
Safe bad() { return [](auto v) { return v; }; }
#else
constexpr Fn fn = lim;
static_assert(fn(4) == 6);
constexpr int run(int (*f)(int), int n) { return f(n); }
constexpr int run_lambda() { return run([](auto n) { return n + 3; }, 4); }
static_assert(run_lambda() == 7);
#endif
''')
    (scope / 'generic-invoker-caller.cpp').write_text('''\
extern "C" int exercise();
extern "C" int invoker(int) __asm__(INVOKER);
int main() { return exercise() != 25 || invoker(8) != 8; }
''')
    cases = (
        ('5typedvENUlTyT_E_', 'IiEES_S_', 'IiEES_S_'),
        ('5fixedvENUlT_E_', 'IiEEiS_', 'IiEEiS_'),
        ('4lrefvENUlRKT_E_', 'IiEEDTcldtdeLKPKS2_0EonclIS_EscS1_fp_EES1_', 'IiEEDaS1_'),
        ('4samevENUlTyT_S_E_', 'IiEEDTcldtdeLKPKS0_0EonclIS_EscOS_fp_scS5_fp0_EES_S_', 'IiEEDaS_S_'),
        ('5multivENUlTyTyT_T0_E_', 'IilEEDTcldtdeLKPKS1_0EonclIS_S0_EscOS_fp_scOS0_fp0_EES_S0_', 'IilEEDaS_S0_'),
        ('3mutvENUlT_E_', 'IiEEDTcldtdeLKPS0_0EonclIS_EscOS_fp_EES_', 'IiEEDaS_'),
        ('4noexvENUlT_E_', 'IiEEDTcldtdeLKPKS0_0EonclIS_EscOS_fp_EES_', 'IiEEDaS_'),
        ('9drop_noexvENUlT_E_', 'IiEEDTcldtdeLKPKS0_0EonclIS_EscOS_fp_EES_', 'IiEEDaS_'),
        ('5vvoidvENUlT_E_', 'IiEEvS_', 'IiEEvS_'),
        ('6packedvENUlTpTyDpT_E_', 'IJilEEEDTcldtdeLKPKS1_0EonclIJS0_EEspscOS_fp_EES0_', 'IJilEEEDaS0_'),
        ('9defaultedvENUlTyTniT_E_', 'IiLi7EEEDTcldtdeLKPKS0_0EonclIS_XT0_EEscOS_fp_EES_', 'IiLi7EEEDaS_'),
        ('8inferredvENUlT_E_', 'IiEEDTcldtdeLKPKS0_0EonclIS_EscOS_fp_EES_', 'IiEEDaS_'),
    )
    clang = shutil.which(os.environ.get('YCXX_CLANGXX', 'clang++-23'))
    private = {
        'spec': '_ZZ15private_genericvENUlT_E0_8__invokeIiEEDaS_',
        'gcc': '_ZZ15private_genericvENUlT_E0_4_FUNIiEEDTcldtdeLKPKS0_0EonclIS_EscOS_fp_EES_',
        'clang': '_ZZ15private_genericvEN3$_08__invokeIiEEDaT_',
    }
    for mode in MODES:
        invoker = '4_FUN' if mode == 'gcc' else '8__invoke'
        names = tuple(f'_ZZ{owner}{invoker}{gcc if mode == "gcc" else other}' for owner, gcc, other in cases)
        flags = ['-fcompat=' + mode]
        command = [*flags, '-c', 'generic-invoker.cpp', '-o', 'generic-invoker.o']
        for target in (LINUX, DARWIN):
            prefix = '_' if target == DARWIN else ''
            outputs = []
            for env in (world.cold, world.daemon):
                result = world.cxx([target, *command], scope, env, timeout=40)
                assert result == (0, b'', b''), (mode, target, result)
                outputs.append((scope / 'generic-invoker.o').read_bytes())
            assert outputs[0] == outputs[1], (mode, target)
            symbols = outputs[0].split(b'\0')
            for name in (*names, private[mode]):
                assert (prefix + name).encode() in symbols, (mode, target, name, [s for s in symbols if b'_ZZ' in s])
            for case in ('', 'CAPTURE', 'RETURN_TYPE', 'NOEXCEPT'):
                define = ['-D' + case] if case else []
                args = [target, *flags, *define, '-fsyntax-only', 'generic-invoker-constant.cpp']
                cold, warm = (world.cxx(args, scope, env, timeout=30) for env in (world.cold, world.daemon))
                assert cold == warm and cold[0] == bool(case), (mode, target, case, cold, warm)
            if (target == LINUX and NATIVE_LINUX) or (target == DARWIN and sys.platform == 'darwin'):
                definition = f'-DINVOKER="{prefix}{names[-1]}"'
                peer = clang if mode == 'clang' else gxx
                if not peer:
                    continue
                subprocess.run([peer, '-std=c++26', definition, 'generic-invoker-caller.cpp', 'generic-invoker.o',
                                '-o', 'generic-invoker'], cwd=scope, check=True, capture_output=True, timeout=40)
                subprocess.run([scope / 'generic-invoker'], cwd=scope, check=True, timeout=20)


def dependent_variable_spellings(world, gxx):
    scope = world.scope
    declarations = '''\
template<bool> struct Box {};
template<bool> struct Other {};
template<bool> struct Extra {};
namespace Ns::Inner { template<class T, int = 5> constexpr bool arg = true; }
namespace Alias = Ns::Inner;
namespace OuterAlias = Ns;
namespace Same = OuterAlias::Inner;
using Ns::Inner::arg;
'''
    written = (
        ('inside', 'Box', 'Alias::arg<T>', 'sr5AliasE', ''),
        ('earlier', 'Box', 'Ns::Inner::arg<T>', 'sr2Ns5InnerE', ''),
        ('other', 'Other', 'Ns::Inner::arg<T>', 'sr2Ns5InnerE', ''),
        ('extra', 'Extra', 'Same::arg<T>', 'sr4SameE', ''),
        ('combined', 'Box', 'Ns::Inner::arg<T, 5>', 'sr2Ns5InnerE', 'Li5E'),
        ('global', 'Other', '::Alias::arg<T>', 'gssr5AliasE', ''),
        ('nested', 'Extra', 'OuterAlias::Inner::arg<T>', 'sr10OuterAlias5InnerE', ''),
        ('bare', 'Extra', 'arg<T>', '', ''),
        ('not_outer', 'Box', '!Ns::Inner::arg<T>', 'sr2Ns5InnerE', ''),
        ('not_alias', 'Box', '!Alias::arg<T>', 'sr5AliasE', ''),
    )
    functions = [f'template<class T> int {fn}({box}<{arg}>)' for fn, box, arg, _, _ in written]
    define = '\n'.join(f'{signature} {{ return {i}; }}' for i, signature in enumerate(functions, 1))
    call = ' + '.join(f'{fn}<int>({box}<{"false" if arg.startswith("!") else "true"}>{{}})' for fn, box, arg, *_ in written)
    (scope / 'variable-spelling-caller.cpp').write_text(declarations + ';\n'.join(functions) +
                                                        f';\nint main() {{ return ({call}) != 55; }}\n')
    (scope / 'variable-redeclaration.cpp').write_text(declarations + '''\
#ifdef BARE
template<class T> void redeclare(Box<arg<T>>) {}
#elif defined(EXPLICIT_DEFAULT)
template<class T> void redeclare(Box<Alias::arg<T, 5>>) {}
#else
template<class T> void redeclare(Box<::Alias::arg<T>>) {}
#endif
template<class T> void redeclare(Box<Ns::Inner::arg<T>>) {}
''')
    command = ['-c', 'variable-spelling.cpp', '-o', 'variable-spelling.o']
    unit = scope / 'variable-spelling.cpp'
    clang = shutil.which(os.environ.get('YCXX_CLANGXX', 'clang++-23'))
    for mode in MODES:
        flags = ['-fcompat=' + mode]
        for target in (LINUX, DARWIN):
            prefix = '_' if target == DARWIN else ''
            for early in ('', 'template<class T> using First = Box<Ns::Inner::arg<T>>;\n',
                          'template<class T> void first() { using First = Box<Ns::Inner::arg<T>>; }\n',
                          'template<class T> struct First { using Shape = Box<Ns::Inner::arg<T>>; };\n', ''):
                first_canonical = bool(early)
                unit.write_text(declarations + early + define + f'\nint instantiate() {{ return {call}; }}\n')
                outputs = []
                for env in (world.daemon, world.cold):
                    result = world.cxx([target, *flags, *command], scope, env, timeout=40)
                    assert result == (0, b'', b''), (mode, target, first_canonical, result)
                    outputs.append((scope / 'variable-spelling.o').read_bytes())
                assert outputs[0] == outputs[1], (mode, target, first_canonical)
                names = outputs[0].split(b'\0')
                for fn, box, arg, scope_name, default in written:
                    if mode == 'gcc':
                        scope_name, default = '', 'Li5E'
                    elif mode == 'clang':
                        if fn in ('inside', 'earlier'):
                            scope_name = 'sr2Ns5InnerE' if first_canonical else 'sr5AliasE'
                        elif fn == 'global':
                            scope_name = 'sr2Ns5InnerE'
                        elif fn == 'nested':
                            scope_name = 'sr4SameE'
                        elif fn == 'not_alias':
                            scope_name = 'sr2Ns5InnerE'
                    negation = 'nt' if arg.startswith('!') else ''
                    expected = f'{prefix}_Z{len(fn)}{fn}IiEi{len(box)}{box}IX{negation}{scope_name}3argIT_{default}EEE'.encode()
                    assert expected in names, (mode, target, first_canonical, expected, [n for n in names if b'3arg' in n])
            for case in ('GLOBAL_ALIAS', 'BARE', 'EXPLICIT_DEFAULT'):
                status = mode == 'gcc' or (mode == 'clang' and case == 'GLOBAL_ALIAS')
                results = [world.cxx([target, *flags, f'-D{case}', '-fsyntax-only', 'variable-redeclaration.cpp'],
                                     scope, env, timeout=30) for env in (world.cold, world.daemon)]
                assert results[0] == results[1] and results[0][0] == status, (mode, target, case, results)
                if status:
                    assert b'redefinition' in results[0][2], results

        peer = gxx if mode == 'gcc' else clang if mode == 'clang' else None
        if peer:
            outcome = world.cxx([*flags, *command], scope, world.daemon, timeout=40)
            assert outcome == (0, b'', b''), (mode, outcome)
            subprocess.run([peer, '-std=c++26', 'variable-spelling-caller.cpp', 'variable-spelling.o', '-o', 'variable-spelling'],
                           cwd=scope, check=True, capture_output=True, timeout=40)
            subprocess.run([scope / 'variable-spelling'], cwd=scope, check=True, timeout=20)


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


def placeholder_template_prefixes(world, gxx):
    scope = world.scope
    templates = (
        ('template<auto V> int single()', 'static_cast<int>(V)'),
        ('template<auto... Values> int pack()', '(0 + ... + Values)'),
        ('template<auto V = 9L> int fallback()', 'static_cast<int>(V)'),
        ('template<auto A, auto B = A> int inherits()', 'static_cast<int>(A + B)'),
        ('template<auto N> int deduced(const char (&)[N])', 'static_cast<int>(N)'),
    )
    calls = 'single<4>() + single<4u>() + pack<>() + pack<2, 3L, true>() + fallback<>() + inherits<7u>() + deduced("xyz")'
    definitions = '\n'.join(f'{signature} {{ return {value}; }}' for signature, value in templates)
    declarations = '\n'.join(signature + ';' for signature, _ in templates)
    (scope / 'placeholder-provider.cpp').write_text(definitions + f'\nint instantiate() {{ return {calls}; }}\n')
    (scope / 'placeholder-consumer.cpp').write_text(declarations + f'\nint main() {{ return {calls} - 41; }}\n')
    (scope / 'placeholder-array.cpp').write_text('''\
template<auto N> int gather(const char (&)[N]);
#ifdef LARGE
extern char array[2147483648ULL];
#else
extern char array[2147483647ULL];
#endif
int use() { return gather(array); }
''')
    clang = shutil.which(os.environ.get('YCXX_CLANGXX', 'clang++-23'))
    for mode in MODES:
        flags = ['-fcompat=' + mode]
        empty = '' if mode == 'gcc' else 'TpTnDa'
        array_type = 'i' if mode == 'gcc' else 'm'
        symbols = (
            '_Z6singleITnDaLi4EEiv',
            '_Z6singleITnDaLj4EEiv',
            f'_Z4packI{empty}JEEiv',
            '_Z4packITpTnDaJLi2ELl3ELb1EEEiv',
            '_Z8fallbackITnDaLl9EEiv',
            '_Z8inheritsITnDaLj7ETnDaLj7EEiv',
            f'_Z7deducedITnDaL{array_type}4EEiRAT__Kc',
        )
        command = [*flags, '-c', 'placeholder-provider.cpp', '-o', 'placeholder-provider.o']
        for target in (LINUX, DARWIN):
            prefix = '_' if target == DARWIN else ''
            for env in (world.cold, world.daemon):
                outcome = world.cxx([target, *command], scope, env, timeout=40)
                assert outcome == (0, b'', b''), (mode, target, outcome)
                names = (scope / 'placeholder-provider.o').read_bytes().split(b'\0')
                for symbol in symbols:
                    assert (prefix + symbol).encode() in names, (mode, target, symbol, [n for n in names if b'_Z' in n])
                array_cmd = [target, *flags, '-c', 'placeholder-array.cpp', '-o', 'placeholder-array.o']
                for large in (False, True):
                    args = [*array_cmd, '-DLARGE'] if large else array_cmd
                    result = world.cxx(args, scope, env, timeout=30)
                    if large and mode == 'gcc':
                        assert result[0] == 1, (mode, target, result)
                        continue
                    assert result == (0, b'', b''), (mode, target, result)
                    bound = 2147483648 if large else 2147483647
                    symbol = f'{prefix}_Z6gatherITnDaL{array_type}{bound}EEiRAT__Kc'.encode()
                    names = (scope / 'placeholder-array.o').read_bytes().split(b'\0')
                    assert symbol in names, (mode, target, symbol, [n for n in names if b'_Z' in n])
        peer = gxx if mode == 'gcc' else clang
        if peer:
            result = world.cxx(command, scope, world.daemon, timeout=40)
            assert result == (0, b'', b''), (mode, result)
            subprocess.run([peer, '-std=c++26', '-c', 'placeholder-consumer.cpp', '-o', 'placeholder-consumer.o'],
                           cwd=scope, check=True, capture_output=True, timeout=40)
            subprocess.run([gxx, 'placeholder-provider.o', 'placeholder-consumer.o', '-o', 'placeholder-link'],
                           cwd=scope, check=True, capture_output=True, timeout=40)
            subprocess.run([scope / 'placeholder-link'], cwd=scope, check=True, timeout=20)


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
        for feature in ('PRIVATE_MEMBERS', 'OVERLAPPING_MEMBER', 'NESTED_OVERLAPPING_MEMBER'):
            peer = clang if mode == 'clang' or (mode == 'spec' and feature != 'PRIVATE_MEMBERS') else gxx
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


def concrete_transform_signatures(world):
    scope = world.scope
    (scope / 'concrete-transform.cpp').write_text('''\
enum E : int { item = 7 };
#if defined(TRAILING)
template<class T> auto f(T n) -> __remove_cv(const int) { return n; }
#elif defined(PARAMETER)
template<class T> int apply(__remove_cv(const int) x) { return x; }
template<class T> int f(T n) { return apply<T>(n); }
#elif defined(UNDERLYING)
template<class T> __underlying_type(E) f(T n) { return n; }
#elif defined(ORDINARY_ALIAS)
using Alias = __remove_cv(const int);
template<class T> Alias f(T n) { return n; }
#elif defined(ALIAS_TEMPLATE)
template<class T> using Alias = __remove_cv(const int);
template<class T> Alias<T> f(T n) { return n; }
#elif defined(BOUND_ALIAS)
template<class T> using Alias = __remove_cv(const int);
template<class T> Alias<int> f(T n) { return n; }
#elif defined(CHAINED_ALIAS)
template<class A> using Inner = __remove_cv(const int);
template<class B> using Alias = Inner<B>;
template<class T> Alias<T> f(T n) { return n; }
#elif defined(PARTIAL_ALIAS)
template<class X, class Y> using Alias = __remove_cv(Y);
template<class T> Alias<T, const int> f(T n) { return n; }
#elif defined(POINTER_ALIAS)
int value = 7;
template<class X, class Y> using Alias = __remove_cv(Y)*;
template<class T> Alias<T, const int> f(T) { return &value; }
#elif defined(MEMBER_ALIAS)
template<class X> struct S { template<class Y> using Alias = __remove_cv(const int); };
template<class T> typename S<T>::template Alias<T> f(T n) { return n; }
#elif defined(CLASS_ARGUMENT) || defined(GUARD_CLASS_ARGUMENT)
template<class T> struct Result { T value; };
template<class T> Result<__remove_cv(const int)> f(T n) {
#if defined(GUARD_CLASS_ARGUMENT)
  static T value = n;
  return {value};
#else
  return {n};
#endif
}
#elif defined(CLASS_ALIAS)
template<class X, class Y> struct Result { Y value; };
template<class X> using Alias = Result<X, __remove_cv(const int)>;
template<class T> Alias<T> f(T n) { return {n}; }
#elif defined(GUARD_DYNAMIC) || defined(GUARD_UNINSTANTIATED)
template<class T> __remove_cv(const int) f(T n) { static int value = n; return value; }
#elif defined(GUARD_UNUSED)
template<class T> __remove_cv(const int) f(T n) { static int value = n; return n; }
#elif defined(GUARD_DISCARDED)
template<class T> __remove_cv(const int) f(T n) {
  if constexpr (sizeof(T) == 1) { static int value = n; return value; }
  return n;
}
#elif defined(GUARD_DEPENDENT)
template<class T> __remove_cv(T) f(T n) { static T value = n; return value; }
#elif defined(GUARD_CONSTANT)
template<class T> __remove_cv(const int) f(T n) { static constexpr int value = 1; return n + value; }
#elif defined(ORDINARY_MEMBER)
template<class T> struct S { __remove_cv(const int) apply(int n) { return n; } };
template<class T> int f(T n) { S<T> s; return s.apply(n); }
#elif defined(MEMBER_TEMPLATE)
struct S { template<class T> static __remove_cv(const int) apply(T n) { return n; } };
template<class T> int f(T n) { return S::apply(n); }
#elif defined(BODY)
template<class T> int f(T n) { __remove_cv(const int) value = n; return value; }
#elif defined(HEADER_EXTENSION)
template<class T> __remove_const(const int) f(T n) { return n; }
#elif defined(OVERLOAD)
template<class T> __remove_cv(const int) f(T n);
template<class T> int f(T n) { return n; }
#else
template<class T> __remove_cv(const int) f(T n) { return n; }
#endif
#if defined(GUARD_UNINSTANTIATED)
int use() { return 7; }
#elif defined(POINTER_ALIAS)
int use() { return *f(7); }
#elif defined(CLASS_ARGUMENT) || defined(CLASS_ALIAS) || defined(GUARD_CLASS_ARGUMENT)
int use() { return f(7).value; }
#else
int use() { return f(7); }
#endif
''')
    for mode in MODES:
        flags = ['-fcompat=' + mode]
        for target in (LINUX, DARWIN):
            emitted_cases = ('', 'TRAILING', 'PARAMETER', 'UNDERLYING', 'ALIAS_TEMPLATE', 'MEMBER_TEMPLATE',
                             'CHAINED_ALIAS', 'PARTIAL_ALIAS', 'POINTER_ALIAS', 'CLASS_ARGUMENT', 'CLASS_ALIAS',
                             'GUARD_DYNAMIC', 'GUARD_DEPENDENT', 'GUARD_CLASS_ARGUMENT', 'GUARD_CONSTANT',
                             'GUARD_UNUSED', 'GUARD_DISCARDED')
            folded = ('ORDINARY_ALIAS', 'BOUND_ALIAS', 'MEMBER_ALIAS', 'ORDINARY_MEMBER', 'BODY', 'HEADER_EXTENSION',
                      'GUARD_UNINSTANTIATED')
            for case in (*emitted_cases, *folded, 'OVERLOAD'):
                opts = [target, *flags, *(['-D' + case] if case else [])]
                syntax = [*opts, '-fsyntax-only', 'concrete-transform.cpp']
                native = [*opts, '-c', 'concrete-transform.cpp', '-o', 'concrete-transform.o']
                ambiguous = mode == 'gcc' and case == 'OVERLOAD'
                emitted = mode == 'gcc' and case in emitted_cases
                guard = mode == 'gcc' and case in ('GUARD_DYNAMIC', 'GUARD_DEPENDENT', 'GUARD_CLASS_ARGUMENT', 'GUARD_UNUSED')
                for command, reject in ((syntax, ambiguous or guard), (native, ambiguous or emitted)):
                    cold = world.cxx(command, scope, world.cold, timeout=40)
                    resident = world.cxx(command, scope, world.daemon, timeout=40)
                    assert cold == resident and resident[0] == int(reject), (mode, target, case, cold, resident)
                    if reject and emitted:
                        assert b'built-in type trait' in resident[2], (mode, target, case, resident)


def constraint_expression_symbols(world, gxx):
    scope = world.scope
    declarations = '''\
struct A {
  static constexpr bool value = true;
  static constexpr int number = 3;
  constexpr int f() const noexcept { return 1; }
  int member;
};
template<class T> struct Trait { static constexpr bool value = true; };
template<class T> constexpr bool check() { return true; }
namespace N {
  template<class T> constexpr bool check() { return true; }
  constexpr bool yes = true;
  constexpr bool read(int x) { return x != 0; }
}
namespace Alias = N;
'''
    # The differences here use GCC's primary symbols with its legacy aliases disabled.
    heads = (
        ('(T::value)', 'srT_5value', '', ''),
        ('(Trait<T>::value)', 'sr5TraitIT_EE5value', 'sr5TraitIT_E5value', 'S2_'),
        ('(check<T>())', 'cl5checkIT_EE', '', ''),
        ('(::N::check<T>())', 'clgssr1NE5checkIT_EE', 'cl5checkIT_EE', ''),
        ('(Alias::check<T>())', 'clsr5AliasE5checkIT_EE', 'cl5checkIT_EE', ''),
        ('(noexcept(T{}))', 'nxtlT_E', '', ''),
        ('(T::value ? true : false)', 'qusrT_5valueLb1ELb0E', '', ''),
        ('(bool(T::value))', 'cvbsrT_5value', '', ''),
        ('(static_cast<bool>(T::number))', 'scbsrT_6number', '', ''),
        ('((~T::number != 0) && ((-T::number) < 0))', 'aanecosrT_6numberLi0EltngsrS1_6numberLi0E', '', ''),
        ('(N::yes)', 'L_ZN1NL3yesEE', '', 'T_'),
        ('(N::read(T::number))', 'clL_ZN1N4readEiEsrT_6numberE', 'cl4readsrT_6numberE', 'S1_'),
        ('(T::number > 0 && sizeof(T{}) > 0)', 'aagtsrT_6numberLi0EgtsztlS1_ELi0E', '', ''),
        ('requires { T::number; }', 'rqXsrT_6numberE', '', ''),
        ('requires(T t) { t.f(); }', 'rQT__Xcldt{arg}1fEE', '', ''),
        ('requires(T t) { t.member; }', 'rQT__Xdt{arg}6memberE', '', ''),
        ('requires(T* p) { p->member; }', 'rQPT__Xpt{arg}6memberE', '', ''),
        ('requires(T* p) { { *p }; p[0]; }', 'rQPT__Xde{arg}Xix{arg}Li0EE', '', ''),
        ('requires(T a) { { a.f() } noexcept; }', 'rQT__Xcldt{arg}1fENE', '', ''),
    )
    trailing = (
        ('(sizeof(x) > 0)', 'gtsz{arg}Li0E'),
        ('(noexcept(x.f()))', 'nxcldt{arg}1fE'),
    )
    signatures = [f'template<class T> requires {condition} int h{i:02}(T)' for i, (condition, *_) in enumerate(heads)]
    signatures += [f'template<class T> int t{i:02}(T x) requires {condition}' for i, (condition, _) in enumerate(trailing)]
    main = ' + '.join([f'h{i:02}(A{{}})' for i in range(len(heads))] + [f't{i:02}(A{{}})' for i in range(len(trailing))])
    (scope / 'expression-constraints.cpp').write_text(declarations +
        '\n'.join(f'{signature} {{ return 1; }}' for signature in signatures) +
        f'\nint force() {{ return {main}; }}\n')
    (scope / 'expression-constraints-caller.cpp').write_text(declarations +
        ';\n'.join(signatures) + f';\nint main() {{ return ({main}) != {len(signatures)}; }}\n')
    clang = shutil.which(os.environ.get('YCXX_CLANGXX', 'clang++-23'))
    for mode in MODES:
        command = ['-fcompat=' + mode, '-c', 'expression-constraints.cpp', '-o', 'expression-constraints.o']
        for target in (LINUX, DARWIN):
            prefix = '_' if target == DARWIN else ''
            for env in (world.cold, world.daemon):
                outcome = world.cxx([target, *command], scope, env, timeout=40)
                assert outcome == (0, b'', b''), (mode, target, outcome)
                names = (scope / 'expression-constraints.o').read_bytes().split(b'\0')
                for i, (_, expression, gcc, result) in enumerate(heads):
                    parameter = result if i == 10 or (mode == 'gcc' and result) else 'S2_' if i == 11 else 'S1_'
                    encoded = (gcc if mode == 'gcc' and gcc else expression).format(arg='fL0p_' if mode == 'clang' else 'fp_')
                    symbol = f'{prefix}_Z3h{i:02}I1AQ{encoded}Ei{parameter}'.encode()
                    assert symbol in names, (mode, target, symbol, [n for n in names if b'_Z3h' in n])
                for i, (_, expression) in enumerate(trailing):
                    encoded = expression.format(arg='fL0p_' if mode == 'gcc' else 'fp_')
                    symbol = f'{prefix}_Z3t{i:02}I1AEiT_Q{encoded}'.encode()
                    assert symbol in names, (mode, target, symbol, [n for n in names if b'_Z3t' in n])
        peer = gxx if mode == 'gcc' else clang if mode == 'clang' else None
        if peer:
            args = ['-fabi-compat-version=0'] if mode == 'gcc' else []
            compiled = world.cxx(command, scope, world.daemon, timeout=40)
            assert compiled == (0, b'', b''), (mode, compiled)
            subprocess.run([peer, '-std=c++26', *args, 'expression-constraints-caller.cpp', 'expression-constraints.o',
                            '-o', 'expression-constraints'], cwd=scope, check=True, capture_output=True, timeout=40)
            subprocess.run([scope / 'expression-constraints'], cwd=scope, check=True, timeout=20)


def overlapping_member_layout(world, gxx):
    scope = world.scope
    shape = '''\
struct Empty {};
struct WithEmpty { int value; char byte; [[no_unique_address]] Empty a, b; };
struct OnlyEmpty { [[no_unique_address]] Empty a, b; };
struct Member { int value; char byte; ~Member() noexcept {} };
struct WithMember { [[no_unique_address]] Member word; char last; };
static_assert(sizeof(WithEmpty) == 8 && __builtin_offsetof(WithEmpty, a) == 0 && __builtin_offsetof(WithEmpty, b) == 5);
static_assert(sizeof(OnlyEmpty) == 2 && __builtin_offsetof(OnlyEmpty, a) == 0 && __builtin_offsetof(OnlyEmpty, b) == 1);
static_assert(sizeof(WithMember) == 8 && __builtin_offsetof(WithMember, last) == 5);
'''
    (scope / 'overlap-reader.cpp').write_text(shape + '''\
extern "C" int read_overlapping(const WithMember& value) {
  return value.word.value + value.word.byte + value.last;
}
''')
    (scope / 'overlap-peer.cpp').write_text(shape + '''\
extern "C" int read_overlapping(const WithMember&);
int main() { return read_overlapping({{17, 19}, 23}) != 59; }
''')
    subprocess.run([gxx, '-std=c++26', '-c', 'overlap-peer.cpp', '-o', 'overlap-peer.o'],
                   cwd=scope, check=True, capture_output=True, timeout=40)
    for mode in MODES:
        for target in (LINUX, DARWIN):
            command = [target, '-fcompat=' + mode, '-c', 'overlap-reader.cpp', '-o', 'overlap-reader.o']
            for env in (world.cold, world.daemon):
                result = world.cxx(command, scope, env, timeout=40)
                assert result == (0, b'', b''), (mode, target, result)
                if (target == LINUX and NATIVE_LINUX) or (target == DARWIN and sys.platform == 'darwin'):
                    subprocess.run([gxx, 'overlap-reader.o', 'overlap-peer.o', '-o', 'overlap-calls'],
                                   cwd=scope, check=True, capture_output=True, timeout=40)
                    subprocess.run([scope / 'overlap-calls'], cwd=scope, check=True, timeout=20)


def late_defaulted_special_members(world, gxx):
    scope = world.scope
    (scope / 'const-defaulted.cpp').write_text('''\
struct First { int value; char byte; First() = default; };
#ifdef CHECK_CONST
const First plain;
#else
int first() { First value{}; return value.value; }
#endif
''')
    (scope / 'late-defaulted.cpp').write_text('''\
struct Ctor { int value = 17; char byte = 19; Ctor() noexcept; };
struct Assign { int value; char byte; Assign& operator=(const Assign&) noexcept; };
struct DerivedAssign : Assign { char tail; };
struct Dtor { int value; char byte; ~Dtor() noexcept; };
struct Copy { int value; char byte; Copy() = default; Copy(const Copy&) noexcept; };
struct MutableCopy { int value; char byte; MutableCopy() = default; MutableCopy(MutableCopy&) noexcept; };
struct Move { int value; char byte; Move() = default; Move(Move&&) noexcept; };
struct DerivedCopy : Copy { char tail; };
struct DerivedMutableCopy : MutableCopy { char tail; };
struct DerivedMove : Move { char tail; };
static_assert(sizeof(DerivedAssign) == 8);
static_assert(sizeof(DerivedCopy) == 8 && sizeof(DerivedMutableCopy) == 8 && sizeof(DerivedMove) == 8);
#if defined(PROVIDER)
Ctor::Ctor() noexcept = default;
Assign& Assign::operator=(const Assign&) noexcept = default;
Dtor::~Dtor() noexcept = default;
Copy::Copy(const Copy&) noexcept = default;
MutableCopy::MutableCopy(MutableCopy&) noexcept = default;
Move::Move(Move&&) noexcept = default;
extern "C" int read_late_assignment(const DerivedAssign& value) { return value.value + value.byte + value.tail; }
#else
extern "C" int read_late_assignment(const DerivedAssign&);
int main() {
  Ctor ctor;
  Dtor dtor{7, 11};
  DerivedAssign dst{{0, 0}, 23};
  const Assign src{31, 37};
  Assign& result = static_cast<Assign&>(dst).operator=(src);
  Copy copy_source;
  copy_source.value = 11;
  copy_source.byte = 13;
  MutableCopy mutable_source;
  mutable_source.value = 17;
  mutable_source.byte = 19;
  Move move_source;
  move_source.value = 23;
  move_source.byte = 29;
  Copy copy(copy_source);
  MutableCopy mutable_copy(mutable_source);
  Move move(static_cast<Move&&>(move_source));
  return ctor.value + ctor.byte + dtor.value + dtor.byte != 54 ||
         &result != &dst || dst.tail != 23 || read_late_assignment(dst) != 91 ||
         copy.value != 11 || copy.byte != 13 || mutable_copy.value != 17 || mutable_copy.byte != 19 ||
         move.value != 23 || move.byte != 29;
}
#endif
''')
    (scope / 'late-invalid.cpp').write_text('''\
#ifdef COPY
struct Base { Base(volatile Base&); };
Base::Base(volatile Base&) = default;
#else
struct Base { Base(const Base&&); };
Base::Base(const Base&&) = default;
#endif
''')
    clang = shutil.which(os.environ.get('YCXX_CLANGXX', 'clang++-23'))
    for mode in MODES:
        for target in (LINUX, DARWIN):
            for options in ([], ['-DCOPY']):
                args = [target, '-fcompat=' + mode, *options, '-fsyntax-only', 'late-invalid.cpp']
                cold, warm = (world.cxx(args, scope, env, timeout=40) for env in (world.cold, world.daemon))
                assert cold == warm and warm[0] == 1, (mode, target, cold, warm)
            for options, rejected in (([], False), (['-DCHECK_CONST'], True)):
                args = [target, '-fcompat=' + mode, *options, '-fsyntax-only', 'const-defaulted.cpp']
                cold, warm = (world.cxx(args, scope, env, timeout=40) for env in (world.cold, world.daemon))
                assert cold == warm and warm[0] == int(rejected), (mode, target, cold, warm)
            command = [target, '-fcompat=' + mode, '-DPROVIDER', '-c', 'late-defaulted.cpp', '-o', 'late-defaulted.o']
            outputs = []
            for env in (world.cold, world.daemon):
                result = world.cxx(command, scope, env, timeout=40)
                assert result == (0, b'', b''), (mode, target, result)
                outputs.append((scope / 'late-defaulted.o').read_bytes())
            assert outputs[0] == outputs[1], (mode, target)
            names = outputs[1].split(b'\0')
            prefix = b'_' if target == DARWIN else b''
            for symbol in (b'_ZN4CopyC1ERKS_', b'_ZN11MutableCopyC1ERS_', b'_ZN4MoveC1EOS_'):
                assert prefix + symbol in names, (mode, target, symbol)
            if (target == LINUX and NATIVE_LINUX) or (target == DARWIN and sys.platform == 'darwin'):
                peer = clang if mode == 'clang' else gxx
                if not peer:
                    continue
                subprocess.run([peer, '-std=c++26', 'late-defaulted.cpp', 'late-defaulted.o', '-o', 'late-defaulted'],
                               cwd=scope, check=True, capture_output=True, timeout=40)
                subprocess.run([scope / 'late-defaulted'], cwd=scope, check=True, timeout=20)


def fold_constraint_symbols(world, gxx):
    scope = world.scope
    declarations = '''\
template<class T> concept Sized = sizeof(T) >= 2;
template<class... T> requires ((sizeof(T) >= 2) && ...) int right(T...) BODY(1)
template<class... T> requires (... && (sizeof(T) >= 2)) int left(T...) BODY(2)
template<class... T> requires ((sizeof(T) >= 2) && ... && true) int binary_right(T...) BODY(3)
template<class... T> requires (true && ... && (sizeof(T) >= 2)) int binary_left(T...) BODY(4)
template<class... T> requires (Sized<T> && ...) int concepts(T...) BODY(5)
template<class... T> requires (sizeof...(T) == 2) int count_types(T...) BODY(6)
template<class... T> int count_values(T... v) requires (sizeof...(v) == 2) BODY(7)
template<class... T> int req(T...) requires requires { requires (Sized<T> && ...); } BODY(8)
'''
    exercise = '''\
right(7, 3L) + left(7, 3L) + binary_right(7, 3L) + binary_left(7, 3L) +
concepts(7, 3L) + count_types(7, 3L) + count_values(7, 3L) + req(7, 3L)
'''
    (scope / 'fold-constraints.cpp').write_text(
        '#define BODY(N) { return N; }\n' + declarations + 'int use() { return ' + exercise + '; }\n')
    (scope / 'fold-constraints-caller.cpp').write_text(
        '#define BODY(N) ;\n' + declarations + 'int main() { return (' + exercise + ') != 36; }\n')
    shared = (
        '_Z5rightIJilEQfraagestT_Li2EEiDpS0_',
        '_Z4leftIJilEQflaagestT_Li2EEiDpS0_',
        '_Z12binary_rightIJilEQfRaagestT_Li2ELb1EEiDpS0_',
        '_Z11binary_leftIJilEQfLaaLb1EgestT_Li2EEiDpS0_',
        '_Z8conceptsIJilEQfraa5SizedIT_EEiDpS0_',
        '_Z11count_typesIJilEQeqsZT_Li2EEiDpT_',
        '_Z3reqIJilEEiDpT_QrqQfraa5SizedIS0_EE',
    )
    clang = shutil.which(os.environ.get('YCXX_CLANGXX', 'clang++-23'))
    for mode in MODES:
        level = 'fL0p_' if mode == 'gcc' else 'fp_'
        names = (*shared, f'_Z12count_valuesIJilEEiDpT_QeqsZ{level}Li2E')
        for target in (LINUX, DARWIN):
            command = [target, '-fcompat=' + mode, '-c', 'fold-constraints.cpp', '-o', 'fold-constraints.o']
            outputs = []
            for env in (world.cold, world.daemon):
                result = world.cxx(command, scope, env, timeout=40)
                assert result == (0, b'', b''), (mode, target, result)
                outputs.append((scope / 'fold-constraints.o').read_bytes())
            assert outputs[0] == outputs[1], (mode, target)
            prefix = '_' if target == DARWIN else ''
            symbols = outputs[0].split(b'\0')
            for name in names:
                assert (prefix + name).encode() in symbols, (mode, target, name, [s for s in symbols if b'_Z' in s])
            if (target == LINUX and NATIVE_LINUX) or (target == DARWIN and sys.platform == 'darwin'):
                peer = gxx if mode == 'gcc' else clang
                if not peer:
                    continue
                args = ['-fabi-compat-version=0'] if mode == 'gcc' else []
                subprocess.run([peer, '-std=c++26', *args, 'fold-constraints-caller.cpp', 'fold-constraints.o',
                                '-o', 'fold-constraints'], cwd=scope, check=True, capture_output=True, timeout=40)
                subprocess.run([scope / 'fold-constraints'], cwd=scope, check=True, timeout=20)


def fold_constraint_ordering(world):
    scope = world.scope
    (scope / 'fold-ordering.cpp').write_text('''\
template<class T> concept A = sizeof(T) >= 2;
template<class T> concept B = A<T> && sizeof(T) >= 4;
#if defined(NAMED)
template<class... T> concept Weak = (A<T> && ...);
template<class... T> concept Strong = (... && B<T>);
template<class... T> requires Weak<T...> constexpr int choose(T...) { return 1; }
template<class... T> requires Strong<T...> constexpr int choose(T...) { return 2; }
#elif defined(DIFFERENT_PACK)
template<class... T> struct Pack {};
template<class... T, class... U> requires (A<T> && ...) constexpr int choose(Pack<T...>, Pack<U...>) { return 1; }
template<class... T, class... U> requires (B<U> && ...) constexpr int choose(Pack<T...>, Pack<U...>) { return 2; }
#elif defined(OR)
template<class... T> requires (A<T> || ...) constexpr int choose(T...) { return 1; }
template<class... T> requires (... || B<T>) constexpr int choose(T...) { return 2; }
#elif defined(DIFFERENT_OPERATOR)
template<class... T> requires (A<T> || ...) constexpr int choose(T...) { return 1; }
template<class... T> requires (B<T> && ...) constexpr int choose(T...) { return 2; }
#else
template<class... T> requires (A<T> && ...) constexpr int choose(T...) { return 1; }
template<class... T> requires (... && B<T>) constexpr int choose(T...) { return 2; }
#endif
#if defined(DIFFERENT_PACK)
static_assert(choose(Pack<int>{}, Pack<int>{}) == 2);
#else
static_assert(choose(7) == 2);
#endif
''')
    for mode in MODES:
        for target in (LINUX, DARWIN):
            for case in ('', 'NAMED', 'OR', 'DIFFERENT_OPERATOR', 'DIFFERENT_PACK'):
                flags = ['-D' + case] if case else []
                command = [target, '-fcompat=' + mode, *flags, '-fsyntax-only', 'fold-ordering.cpp']
                cold, warm = (world.cxx(command, scope, env, timeout=40) for env in (world.cold, world.daemon))
                ambiguous = mode == 'gcc' or case in ('DIFFERENT_OPERATOR', 'DIFFERENT_PACK')
                assert cold == warm and warm[0] == int(ambiguous), (mode, target, case, cold, warm)
                if ambiguous:
                    assert b'ambiguous' in warm[2], (mode, target, case, warm[2])


def fold_constraint_satisfaction(world):
    scope = world.scope
    (scope / 'fold-satisfaction.cpp').write_text('''\
#if defined(INTEGER) || defined(EMPTY)
template<int... N> requires (N && ...) constexpr int choose() { return 7; }
#ifdef EMPTY
static_assert(choose<>() == 7);
#else
static_assert(choose<1, 2>() == 7);
#endif
#elif defined(COMPARISON)
template<int... N> requires ((N > 0) && ...) constexpr int choose() { return 7; }
static_assert(choose<1, 2>() == 7);
#else
template<class T> constexpr bool check() {
  static_assert(sizeof(T) != 8, "later fold element instantiated");
  return sizeof(T) > 1;
}
#ifdef OR
template<class... T> requires (check<T>() || ...) constexpr int choose(T...) { return 7; }
static_assert(choose(2, 2LL) == 7);
#else
template<class... T> requires (check<T>() && ...) constexpr int choose(T...) { return 7; }
constexpr int choose(double, double) { return 2; }
#ifdef NEED_LATER
static_assert(choose(2, 2LL) == 7);
#else
static_assert(choose('x', 2LL) == 2);
#endif
#endif
#endif
''')
    for mode in MODES:
        for target in (LINUX, DARWIN):
            for case in ('INTEGER', 'COMPARISON', 'EMPTY', '', 'OR', 'NEED_LATER'):
                define = ['-D' + case] if case else []
                args = [target, '-fcompat=' + mode, *define, '-fsyntax-only', 'fold-satisfaction.cpp']
                cold, warm = (world.cxx(args, scope, env, timeout=40) for env in (world.cold, world.daemon))
                rejected = case == 'NEED_LATER' or (mode == 'gcc' if case in ('', 'OR') else
                                                    mode != 'gcc' and case == 'INTEGER')
                assert cold == warm and warm[0] == int(rejected), (mode, target, case, cold, warm)
                if rejected:
                    diagnostic = b'constraint' if case == 'INTEGER' else b'later fold element instantiated'
                    assert diagnostic in warm[2], (mode, target, case, warm[2])


def binary_fold_constraints(world):
    scope = world.scope
    (scope / 'binary-fold.cpp').write_text('''\
#ifndef CASE
#define CASE 0
#endif
#if CASE == 0
template<int... N> requires (N && ... && true) constexpr int f() { return 7; }
static_assert(f<1, 2>() == 7);
#elif CASE == 1
template<int... N> requires (true && ... && N) constexpr int f() { return 7; }
static_assert(f<1, 2>() == 7);
#elif CASE == 2
template<int... N> requires ((N > 0) && ... && 1) constexpr int f() { return 7; }
static_assert(f<1, 2>() == 7);
#elif CASE == 3
template<int... N> requires (1 && ... && (N > 0)) constexpr int f() { return 7; }
static_assert(f<1, 2>() == 7);
#elif CASE >= 4 && CASE <= 7
template<class T> constexpr bool late() {
  static_assert(sizeof(T) != 8, "late substitution");
  return true;
}
template<class... T>
#if CASE == 4
  requires (false && ... && late<T>())
#elif CASE == 5
  requires (late<T>() && ... && false)
#elif CASE == 6
  requires (true || ... || late<T>())
#else
  requires (late<T>() || ... || true)
#endif
constexpr int f(T...) { return 7; }
constexpr int f(double) { return 2; }
static_assert(f(1LL) == (CASE < 6 ? 2 : 7));
#elif CASE == 8
template<class T> concept A = sizeof(T) > 0;
template<class T> concept B = A<T> && (sizeof(T) > 1);
template<class... T> requires (A<T> && ... && true)
constexpr int f(T...) { return 2; }
template<class... T> requires (B<T> && ... && true)
constexpr int f(T...) { return 7; }
static_assert(f(42) == 7);
#elif CASE == 9
template<class T> concept A = sizeof(T) > 0;
template<class T> concept B = A<T> && (sizeof(T) > 1);
template<class... T> requires (A<T> && ...)
constexpr int f(T...) { return 2; }
template<class... T> requires (true && ... && B<T>)
constexpr int f(T...) { return 7; }
static_assert(f(42) == 7);
#elif CASE == 10
template<class T> concept A = sizeof(T) > 0;
template<class T> concept B = A<T> && (sizeof(T) > 1);
template<class... T> requires (A<T> && ... && true)
constexpr int f(T...) { return 2; }
template<class... T> requires (B<T> && ...)
constexpr int f(T...) { return 7; }
static_assert(f(42) == 7);
#elif CASE == 11
template<class T> concept A = sizeof(T) > 0;
template<class T> concept B = A<T> && (sizeof(T) > 1);
template<class... T> requires (false || ... || A<T>)
constexpr int f(T...) { return 2; }
template<class... T> requires (B<T> || ...)
constexpr int f(T...) { return 7; }
static_assert(f(42) == 7);
#elif CASE >= 12
template<class T> constexpr bool late() {
  static_assert(sizeof(T) != 8, "late substitution");
  return true;
}
template<class... T> requires (((sizeof(T) < 4) && late<T>()) && ...)
constexpr int f(T...) { return 7; }
constexpr int f(double) { return 2; }
static_assert(f(1LL) == 2);
#endif
''')
    for mode in MODES:
        for target in (LINUX, DARWIN):
            for case in range(13):
                args = [target, '-fcompat=' + mode, f'-DCASE={case}', '-fsyntax-only', 'binary-fold.cpp']
                cold, warm = (world.cxx(args, scope, env, timeout=40) for env in (world.cold, world.daemon))
                rejected = case >= 4 if mode == 'gcc' else case not in (4, 6, 9, 11, 12)
                assert cold == warm and warm[0] == int(rejected), (mode, target, case, cold, warm)
                if rejected:
                    diagnostic = b'constraint' if case < 4 else b'late substitution' if case < 8 or case == 12 else b'ambiguous'
                    assert diagnostic in warm[2], (mode, target, case, warm[2])


def fold_constraint_substitution_failures(world):
    scope = world.scope
    (scope / 'fold-sfinae.cpp').write_text('''\
struct Missing {};
struct Yes { static constexpr bool value = true; };
struct No { static constexpr bool value = false; };
#if CASE == 0
template<class... T> requires (T::value || ...)
constexpr int f(T...) { return 7; }
template<class... T> constexpr int f(T...) { return 2; }
static_assert(f(Missing{}, Yes{}) == 7);
#elif CASE == 1
template<class... T> requires (T::value && ...)
constexpr int f(T...) { return 7; }
template<class... T> constexpr int f(T...) { return 2; }
static_assert(f(Missing{}, Yes{}) == 2);
#elif CASE == 2
template<class... T> struct Pack {};
template<class... T, class... U> requires ((sizeof(T) == sizeof(U)) && ...)
constexpr int f(Pack<T...>, Pack<U...>) { return 7; }
template<class... T> constexpr int f(T...) { return 2; }
static_assert(f(Pack<int, long>{}, Pack<int, long>{}) == 7);
static_assert(f(Pack<int, long>{}, Pack<int>{}) == 2);
#endif
''')
    for mode in MODES:
        for target in (LINUX, DARWIN):
            for case in range(3):
                args = [target, '-fcompat=' + mode, f'-DCASE={case}', '-fsyntax-only', 'fold-sfinae.cpp']
                cold, warm = (world.cxx(args, scope, env, timeout=40) for env in (world.cold, world.daemon))
                assert cold == warm and warm[0] == int(mode == 'gcc' and case == 0), (mode, target, case, cold, warm)
                if warm[0]:
                    assert b'static assertion' in warm[2], warm[2]


def constrained_parameter_packs(world, gxx):
    scope = world.scope
    (scope / 'constrained-pack.cpp').write_text('''\
template<class T> concept Sized = sizeof(T) >= 2;
template<class T> concept Wide = Sized<T> && sizeof(T) >= 4;
template<Sized... T> constexpr int choose(T...) { return 1; }
#ifdef ORDER
template<Wide... T> constexpr int choose(T...) { return 2; }
static_assert(choose(7) == 2);
#elif defined(UNSATISFIED)
static_assert(choose('a') == 1);
#else
static_assert(choose(7) == 1);
static_assert(choose<>() == 1);
#endif
''')
    common = 'template<class T> concept Sized = sizeof(T) >= 2;\n'
    (scope / 'pack-provider.cpp').write_text(common + '''
template<Sized... T> int f(T...) { return sizeof...(T); }
int actual() { return f(1, 2L); }
''')
    (scope / 'pack-caller.cpp').write_text(common + '''
template<Sized... T> int f(T...);
int main() { return f(1, 2L) != 2; }
''')
    clang = shutil.which(os.environ.get('YCXX_CLANGXX', 'clang++-23'))
    for mode in MODES:
        for target in (LINUX, DARWIN):
            flags = [target, '-fcompat=' + mode]
            for case in ('', 'ORDER', 'UNSATISFIED'):
                args = [*flags, '-fsyntax-only', *(['-D' + case] if case else []), 'constrained-pack.cpp']
                cold, warm = (world.cxx(args, scope, env, timeout=40) for env in (world.cold, world.daemon))
                rejected = case == 'UNSATISFIED' or (case == 'ORDER' and mode == 'gcc')
                assert cold == warm and warm[0] == int(rejected), (mode, target, case, cold, warm)
                if case == 'ORDER' and rejected:
                    assert b'ambiguous' in warm[2], warm[2]
            outputs = []
            for env in (world.cold, world.daemon):
                args = [*flags, '-c', 'pack-provider.cpp', '-o', 'pack-provider.o']
                result = world.cxx(args, scope, env, timeout=40)
                assert result == (0, b'', b''), (mode, target, result)
                outputs.append((scope / 'pack-provider.o').read_bytes())
            symbol = (b'_' if target == DARWIN else b'') + b'_Z1fITpTk5SizedJilEEiDpT_'
            assert outputs[0] == outputs[1] and symbol in outputs[1].split(b'\0'), (mode, target, symbol)
            peer = gxx if mode == 'gcc' else clang
            if peer and ((target == LINUX and NATIVE_LINUX) or (target == DARWIN and sys.platform == 'darwin')):
                abi = ['-fabi-compat-version=0'] if mode == 'gcc' else []
                subprocess.run([peer, '-std=c++26', *abi, 'pack-caller.cpp', 'pack-provider.o', '-o', 'pack-app'],
                               cwd=scope, check=True, capture_output=True, timeout=40)
                subprocess.run([scope / 'pack-app'], cwd=scope, check=True, timeout=20)


def concept_template_parameters(world):
    scope = world.scope
    (scope / 'concept-pack.cpp').write_text('''\
template<class T> concept A = sizeof(T) > 0;
template<class T> concept B = A<T> && (sizeof(T) > 1);
template<class T> concept C = sizeof(T) >= 1;
template<class T> concept D = C<T> && (sizeof(T) > 2);
#if CASE == 0
template<class T, template<class> concept... Cs>
concept all_of = (Cs<T> && ...);
template<class T> requires all_of<T, A, C>
constexpr int f(T) { return 1; }
template<class T> requires all_of<T, B, D>
constexpr int f(T) { return 2; }
static_assert(all_of<int>);
static_assert(f(1) == 2);
#elif CASE == 1
template<class T, template<class> concept Cc> concept apply = Cc<T>;
static_assert(apply<int, A>);
#elif CASE == 2
template<A... T> constexpr int f(T...) { return 1; }
template<B... T> constexpr int f(T...) { return 2; }
static_assert(f(42) == 2);
#elif CASE == 3
template<A... T> constexpr int f(T...) { return 1; }
static_assert(f(42) == 1);
#elif CASE == 4
template<class T, template<class> concept... Cs>
concept all_of = (Cs<T> && ...);
static_assert(all_of<int>);
static_assert(all_of<int, A, B, D>);
static_assert(!all_of<char, A, B>);
#elif CASE == 5
template<class T, template<class> concept... Cs>
concept any_of = (... || Cs<T>);
static_assert(!any_of<int>);
static_assert(any_of<char, B, A>);
static_assert(!any_of<char, B, D>);
#elif CASE == 6
template<class T, template<class> concept Cc> struct Wrap {};
template <class... T, template<class> concept... Cs>
int f(Wrap<T, Cs>...) requires (Cs<T> && ...);
#endif
''')
    (scope / 'concept-pack-symbol.cpp').write_text('''\
template<class T> concept Check = sizeof(T) >= 4;
template<class T> concept Second = sizeof(T) >= 2;
#if CASE == 0
template<template<class> concept C> int f(int value) { return value; }
int use() { return f<Check>(7); }
#elif CASE == 1
template<template<class> concept... C> int f(int value) { return value + sizeof...(C); }
int use() { return f<Check, Second>(7); }
#elif CASE == 2
template<template<class> concept C> requires C<int> int f(int value) { return value; }
int use() { return f<Check>(7); }
#elif CASE == 3
template<template<class> concept... C> requires (C<int> && ...) int f(int value) { return value; }
int use() { return f<Check, Second>(7); }
#endif
''')
    expected = (
        '_Z1fI5CheckEii',
        '_Z1fIJ5Check6SecondEEii',
        '_Z1fI5CheckQ1CIiEEii',
        '_Z1fIJ5Check6SecondEQfraa1CIiEEii',
    )
    clang = shutil.which(os.environ.get('YCXX_CLANGXX', 'clang++-23'))
    for mode in MODES:
        for target in (LINUX, DARWIN):
            flags = [target, '-fcompat=' + mode]
            for case in (0, 1, 4, 5, 6):
                args = [*flags, f'-DCASE={case}', '-fsyntax-only', 'concept-pack.cpp']
                cold, warm = (world.cxx(args, scope, env, timeout=40) for env in (world.cold, world.daemon))
                rejected = mode == 'gcc' or (case == 0 and mode == 'clang') or (case == 6 and mode == 'spec')
                assert cold == warm and warm[0] == int(rejected), (mode, target, case, cold, warm)
                if mode == 'clang' and case == 0:
                    assert b'ambiguous' in warm[2], warm[2]
                if mode == 'spec' and case == 6:
                    assert b'other kinds of packs' in warm[2], warm[2]
            for case, name in enumerate(expected):
                command = [*flags, f'-DCASE={case}', '-c', 'concept-pack-symbol.cpp', '-o', 'concept-pack-symbol.o']
                cold, warm = (world.cxx(command, scope, env, timeout=40) for env in (world.cold, world.daemon))
                assert cold == warm and warm[0] == int(mode == 'gcc'), (mode, target, case, cold, warm)
                if mode == 'gcc':
                    continue
                prefix = '_' if target == DARWIN else ''
                names = (scope / 'concept-pack-symbol.o').read_bytes().split(b'\0')
                assert (prefix + name).encode() in names, (mode, target, case, name, names)
                if clang and ((target == LINUX and NATIVE_LINUX) or (target == DARWIN and sys.platform == 'darwin')):
                    result = 9 if case == 1 else 7
                    (scope / 'concept-pack-caller.cpp').write_text(f'''\
extern "C" int f(int) asm("{prefix}{name}");
int main() {{ return f(7) != {result}; }}
''')
                    subprocess.run([clang, '-std=c++26', 'concept-pack-caller.cpp', 'concept-pack-symbol.o',
                                    '-o', 'concept-pack-call'], cwd=scope, check=True, capture_output=True, timeout=40)
                    subprocess.run([scope / 'concept-pack-call'], cwd=scope, check=True, timeout=20)


def constrained_lambda_members(world, gxx):
    scope = world.scope
    (scope / 'constrained-lambda.cpp').write_text('''\
template<class T> concept Integral = sizeof(T) >= 2;
#if CASE == 0
inline int direct() { return []<class T> requires Integral<T> (T n) { return n; }(7); }
#elif CASE == 1
inline int direct() { return []<Integral T>(T n) { return n; }(7); }
#elif CASE == 2
inline int direct() { return [](Integral auto n) { return n; }(7); }
#elif CASE == 3
inline int direct() { return [](auto n) requires Integral<decltype(n)> { return n; }(7); }
#elif CASE == 4
inline int direct() { return []<class T> requires (sizeof(T) >= 2) (T n) { return n; }(7); }
#elif CASE == 5
inline int direct() { return [](auto n) requires (sizeof(decltype(n)) >= 2) { return n; }(7); }
#elif CASE == 6
inline int direct() { return []<class T> requires Integral<T> (T n) { return n; }('x'); }
#elif CASE == 7
inline int direct() { return []<Integral T>(T n) { return n; }('x'); }
#elif CASE == 8
inline int direct() { return [](Integral auto n) { return n; }('x'); }
#elif CASE == 9
inline int direct() { return [](auto n) requires Integral<decltype(n)> { return n; }('x'); }
#elif CASE == 10
inline int direct() { return []<class T> requires (sizeof(T) >= 2) (T n) { return n; }('x'); }
#elif CASE == 11
inline int direct() { return [](auto n) requires (sizeof(decltype(n)) >= 2) { return n; }('x'); }
#endif
int keep() { return direct(); }
''')
    clang = shutil.which(os.environ.get('YCXX_CLANGXX', 'clang++-23'))
    for mode in MODES:
        level = 'fL0p_' if mode == 'gcc' else 'fp_'
        symbols = (
            '_ZZ6directvENKUlTyQ8IntegralIT_ES_E_clIiEEDaS_',
            '_ZZ6directvENKUlTk8IntegralT_E_clIiEEDaS_',
            '_ZZ6directvENKUlT_E_clIiEEDaS_',
            f'_ZZ6directvENKUlT_E_clIiEEDaS_Q8IntegralIDt{level}EE',
            '_ZZ6directvENKUlTyQgestT_Li2ES_E_clIiEEDaS_',
            f'_ZZ6directvENKUlT_E_clIiEEDaS_QgestDt{level}ELi2E',
        )
        for target in (LINUX, DARWIN):
            peer = gxx if mode == 'gcc' else clang
            prefix = '_' if target == DARWIN else ''
            for case, name in enumerate(symbols):
                outputs = []
                for env in (world.cold, world.daemon):
                    command = [target, '-fcompat=' + mode, f'-DCASE={case}', '-c', 'constrained-lambda.cpp',
                               '-o', 'constrained-lambda.o']
                    result = world.cxx(command, scope, env, timeout=40)
                    assert result == (0, b'', b''), (mode, target, case, result)
                    outputs.append((scope / 'constrained-lambda.o').read_bytes())
                names = outputs[1].split(b'\0')
                assert outputs[0] == outputs[1] and (prefix + name).encode() in names, (
                    mode, target, case, name, [n for n in names if b'_Z' in n])
                rejecting = [target, '-fcompat=' + mode, f'-DCASE={case + 6}', '-fsyntax-only', 'constrained-lambda.cpp']
                cold, warm = (world.cxx(rejecting, scope, env, timeout=40) for env in (world.cold, world.daemon))
                assert cold == warm and warm[0] == 1 and b"no matching function for call to 'operator()'" in warm[2], (
                    mode, target, case, cold, warm)
                if peer and ((target == LINUX and NATIVE_LINUX) or (target == DARWIN and sys.platform == 'darwin')):
                    (scope / 'lambda-caller.cpp').write_text(f'''\
struct EmptyClosure {{}};
extern "C" int call(const EmptyClosure&, int) asm("{prefix}{name}");
int main() {{ return call(EmptyClosure{{}}, 19) != 19; }}
''')
                    abi = ['-fabi-compat-version=0'] if mode == 'gcc' else []
                    subprocess.run([peer, '-std=c++26', *abi, 'lambda-caller.cpp', 'constrained-lambda.o',
                                    '-o', 'lambda-call'], cwd=scope, check=True, capture_output=True, timeout=40)
                    subprocess.run([scope / 'lambda-call'], cwd=scope, check=True, timeout=20)


def written_trait_constraint_symbols(world, gxx):
    scope = world.scope
    (scope / 'trait-constraint.cpp').write_text('''\
struct A { int n; };
#if CASE == 0
#define HEAD (__is_class(T) && true)
#elif CASE == 1
#define HEAD (__is_class(T) || false)
#elif CASE == 2
#define HEAD (!__is_class(T))
#elif CASE == 3
#define HEAD requires { __is_class(T); }
#elif CASE == 4
#define HEAD (__is_class(T) == true)
#elif CASE == 5
#define HEAD (__is_same_as(T, A))
#elif CASE == 6
#define HEAD (__is_same(T, A) && __is_class(T))
#elif CASE == 7
#define HEAD (true && __is_class(T))
#elif CASE == 8
template<class T> concept True = true;
#define HEAD (True<T> && __is_class(T))
#elif CASE == 9
#define HEAD (__is_trivially_copyable(T))
#elif CASE == 10
#define HEAD (__is_class(T) && sizeof(T) > 0)
#elif CASE == 11
#define HEAD (__is_same(T, A) ? true : false)
#endif
#if CASE == 2
#define ARG 1
#else
#define ARG A{}
#endif
#ifdef TRAILING
template<class T> int f(T) requires HEAD { return 1; }
#else
template<class T> requires HEAD int f(T) { return 1; }
#endif
int use() { return f(ARG); }
''')
    def suffix(case, parameter):
        trait = f'u10__is_class{parameter}E'
        same = f'u9__is_same{parameter}S0_E'
        result = (
            f'aa{trait}Lb1E', f'oo{trait}Lb0E', f'nt{trait}', f'rqX{trait}E',
            f'eq{trait}Lb1E', same, f'aa{same}u10__is_classS1_E', f'aaLb1E{trait}',
            'aa4TrueI' + parameter + 'Eu10__is_classS1_E', f'u23__is_trivially_copyable{parameter}E',
            f'aa{trait}gtstS1_Li0E', f'qu{same}Lb1ELb0E',
        )
        return result[case]

    clang = shutil.which(os.environ.get('YCXX_CLANGXX', 'clang++-23'))
    for mode in MODES:
        for target in (LINUX, DARWIN):
            for case in range(12):
                typ, substitution = ('i', 'S0_') if case == 2 else ('1A', 'S1_')
                for trailing in (False, True):
                    flags = [target, '-fcompat=' + mode, f'-DCASE={case}', *(['-DTRAILING'] if trailing else [])]
                    reject = mode == 'gcc' and (trailing or case not in (5, 9))
                    outputs = []
                    for env in (world.cold, world.daemon):
                        syntax = world.cxx([*flags, '-fsyntax-only', 'trait-constraint.cpp'], scope, env, timeout=40)
                        assert syntax == (0, b'', b''), (mode, target, case, trailing, syntax)
                        command = [*flags, '-c', 'trait-constraint.cpp', '-o', 'trait-constraint.o']
                        result = world.cxx(command, scope, env, timeout=40)
                        assert result[0] == int(reject), (mode, target, case, trailing, result)
                        if reject:
                            assert b'built-in type trait' in result[2], result
                        else:
                            outputs.append((scope / 'trait-constraint.o').read_bytes())
                    if reject:
                        continue
                    if mode == 'gcc':
                        suffix_name = f'I{typ}EiT_'
                    elif trailing:
                        suffix_name = f'I{typ}EiT_Q{suffix(case, substitution)}'
                    else:
                        suffix_name = f'I{typ}Q{suffix(case, "T_")}Ei{substitution}'
                    prefix = '_' if target == DARWIN else ''
                    symbol = prefix + '_Z1f' + suffix_name
                    assert outputs[0] == outputs[1] and symbol.encode() in outputs[1].split(b'\0'), (
                        mode, target, case, trailing, symbol)
                    peer = gxx if mode == 'gcc' else clang
                    host = (target == LINUX and NATIVE_LINUX) or (target == DARWIN and sys.platform == 'darwin')
                    if peer and host and case in (5, 9) and not trailing:
                        (scope / 'trait-caller.cpp').write_text(f'''\
struct A {{ int n; }};
extern "C" int call(A) asm("{symbol}");
int main() {{ return call({{42}}) != 1; }}
''')
                        subprocess.run([peer, '-std=c++26', 'trait-caller.cpp', 'trait-constraint.o',
                                        '-o', 'trait-call'], cwd=scope, check=True, capture_output=True, timeout=40)
                        subprocess.run([scope / 'trait-call'], cwd=scope, check=True, timeout=20)


def written_allocation_constraint_symbols(world, gxx):
    scope = world.scope
    (scope / 'allocation-constraint.cpp').write_text('''\
using Size = decltype(sizeof(0));
void* operator new(Size, void*) noexcept;
struct Box {
  int number;
  static constexpr int num = 1;
  constexpr Box(int n = 0) : number(n) {}
  constexpr operator int() const { return number; }
};
#if CASE == 0
#define CONSTRAINT (__is_same(T, Box))
#elif CASE == 1
#define CONSTRAINT (__is_class(T))
#elif CASE == 2
#define CONSTRAINT (__is_convertible(T, int))
#elif CASE == 3
#define CONSTRAINT (__is_constructible(T, int))
#elif CASE == 4
#define CONSTRAINT (__builtin_is_constant_evaluated())
#elif CASE == 5
#define CONSTRAINT (alignof(T) > 0)
#elif CASE == 6
#define CONSTRAINT requires(T* p) { delete p; }
#elif CASE == 7
#define CONSTRAINT requires { new T; }
#elif CASE == 8
#define CONSTRAINT requires(void* p) { new (p) T; }
#elif CASE == 9
#define CONSTRAINT requires { throw T{}; }
#elif CASE == 10
#define CONSTRAINT requires(T k) { (void(k), k); }
#elif CASE == 11
#define CONSTRAINT requires(T k) { k.number += k.number; }
#elif CASE == 12
#define CONSTRAINT requires { &T::number; }
#elif CASE == 13
#define CONSTRAINT requires { int{T::num}; }
#elif CASE == 14
#define CONSTRAINT requires(T* p) { delete[] p; }
#elif CASE == 15
#define CONSTRAINT requires { new T[2]; }
#elif CASE == 16
#define CONSTRAINT requires { new T(7); }
#elif CASE == 17
#define CONSTRAINT requires { new T{7}; }
#elif CASE == 18
#define CONSTRAINT requires(T* p) { ::delete p; }
#elif CASE == 19
#define CONSTRAINT requires { ::new T; }
#endif
template<class T> requires CONSTRAINT
int f(T) { return 1; }
#if CASE == 9 || CASE == 14 || CASE == 15
int use() { return f(7); }
#else
int use() { return f(Box{}); }
#endif
''')
    clang = shutil.which(os.environ.get('YCXX_CLANGXX', 'clang++-23'))
    for mode in MODES:
        arg = 'fL0p_' if mode == 'clang' else 'fp_'
        array = 'T_' if mode == 'clang' else 'A2_T_'
        expected = {
            6: f'rQPT__Xdl{arg}E',
            7: 'rqXnw_T_EE',
            8: f'rQPv_Xnw{arg}_T_EE',
            9: 'rqXtwtlT_EE',
            10: f'rQT__Xcmcvv{arg}{arg}E',
            11: f'rQT__XpLdt{arg}6numberdt{arg}6numberE',
            12: 'rqXadsrT_6numberE',
            13: 'rqXtlisrT_3numEE',
            14: f'rQPT__Xda{arg}E',
            15: f'rqXna_{array}EE',
            16: 'rqXnw_T_piLi7EEE',
            17: 'rqXnw_T_ilLi7EEE',
            18: f'rQPT__Xgsdl{arg}E',
            19: 'rqXgsnw_T_EE',
        }
        for target in (LINUX, DARWIN):
            for case, clause in expected.items():
                command = [target, '-fcompat=' + mode, f'-DCASE={case}', '-c', 'allocation-constraint.cpp',
                           '-o', 'allocation-constraint.o']
                outputs = []
                for env in (world.cold, world.daemon):
                    result = world.cxx(command, scope, env, timeout=40)
                    assert result == (0, b'', b''), (mode, target, case, result)
                    outputs.append((scope / 'allocation-constraint.o').read_bytes())
                prefix = '_' if target == DARWIN else ''
                scalar = case in (9, 14, 15)
                argument = 'i' if scalar else '3Box'
                parameter = 'S0_' if scalar else 'S2_' if case == 8 else 'S1_'
                name = f'{prefix}_Z1fI{argument}Q{clause}Ei{parameter}'
                names = outputs[1].split(b'\0')
                assert outputs[0] == outputs[1] and name.encode() in names, (
                    mode, target, case, name, [n for n in names if b'_Z1f' in n])
                peer = gxx if mode == 'gcc' else clang
                host = (target == LINUX and NATIVE_LINUX) or (target == DARWIN and sys.platform == 'darwin')
                if peer and host and case in (6, 11, 15):
                    argument_type = 'int' if scalar else 'Box'
                    (scope / 'allocation-caller.cpp').write_text(f'''\
struct Box {{
  int number;
  static constexpr int num = 1;
  constexpr Box(int n = 0) : number(n) {{}}
  constexpr operator int() const {{ return number; }}
}};
extern "C" int call({argument_type}) asm("{name}");
int main() {{ return call({argument_type}{{7}}) != 1; }}
''')
                    subprocess.run([peer, '-std=c++26', 'allocation-caller.cpp', 'allocation-constraint.o',
                                    '-o', 'allocation-call'], cwd=scope, check=True, capture_output=True, timeout=40)
                    subprocess.run([scope / 'allocation-call'], cwd=scope, check=True, timeout=20)


def over_width_bit_fields(world, gxx):
    scope = world.scope
    fixture = '''\
struct Wide { unsigned first : 40; char byte; };
struct WideShort { unsigned short first : 40; char byte; };
struct WideChar { unsigned char first : 40; char byte; };
struct Huge { unsigned long long first : 128; char byte; };
struct Private {
private:
  unsigned first : 40;
  char byte;
};
struct Derived : Huge { char last; };
struct PrivateDerived : Private { char last; };
static_assert(sizeof(Wide) == 8 && alignof(Wide) == 4 && __builtin_offsetof(Wide, byte) == 5);
static_assert(sizeof(WideShort) == 8 && alignof(WideShort) == 4 && __builtin_offsetof(WideShort, byte) == 5);
static_assert(sizeof(WideChar) == 8 && alignof(WideChar) == 4 && __builtin_offsetof(WideChar, byte) == 5);
static_assert(alignof(Huge) == HUGE_ALIGNMENT && sizeof(Huge) == HUGE_SIZE);
static_assert(sizeof(Derived) == HUGE_SIZE + HUGE_ALIGNMENT);
static_assert(sizeof(PrivateDerived) == PRIVATE_SIZE);
extern "C" unsigned long long read_big(Huge&);
extern "C" unsigned read_small(Wide&, WideShort&, WideChar&);
extern "C" char replace_tail(Derived&);
'''
    (scope / 'bit-fields-provider.cpp').write_text(fixture + '''\
unsigned long long read_big(Huge& object) {
  unsigned long long previous = object.first;
  object.first = 0x123456789abcdef0ULL;
  return previous + object.byte;
}
unsigned read_small(Wide& wide, WideShort& half, WideChar& byte) {
  unsigned result = wide.first + half.first + byte.first;
  wide.first = 17;
  half.first = 19;
  byte.first = 23;
  return result;
}
char replace_tail(Derived& object) {
  const char previous = object.last;
  object.last = 77;
  return previous;
}
''')
    (scope / 'bit-fields-peer.cpp').write_text(fixture + '''\
int main() {
  Huge h{0x100000002ULL, 5};
  if (read_big(h) != 0x100000007ULL || h.first != 0x123456789abcdef0ULL || h.byte != 5) return 1;
  Derived d{{0x100000002ULL, 5}, 31};
  if (replace_tail(d) != 31 || d.last != 77 || d.first != 0x100000002ULL || d.byte != 5) return 1;
  Wide upper{7, 11};
  WideShort half{13, 21};
  WideChar byte{29, 31};
  return read_small(upper, half, byte) != 49 || upper.first != 17 || half.first != 19 || byte.first != 23 ||
         upper.byte != 11 || half.byte != 21 || byte.byte != 31;
}
''')
    clang = shutil.which(os.environ.get('YCXX_CLANGXX', 'clang++-23'))
    for mode in MODES:
        for target in (LINUX, DARWIN):
            huge = 24 if mode == 'clang' and target == LINUX else 32
            alignment = 8 if huge == 24 else 16
            private = 12 if mode == 'clang' and target == DARWIN else 8
            macros = [f'-DHUGE_SIZE={huge}', f'-DHUGE_ALIGNMENT={alignment}', f'-DPRIVATE_SIZE={private}']
            command = [target, '-fcompat=' + mode, *macros, '-c', 'bit-fields-provider.cpp', '-o', 'bit-fields.o']
            outputs = []
            for env in (world.cold, world.daemon):
                result = world.cxx(command, scope, env, timeout=40)
                assert result[0] == 0, (mode, target, result)
                outputs.append((scope / 'bit-fields.o').read_bytes())
            assert outputs[0] == outputs[1], (mode, target)
            peer = clang if mode == 'clang' else gxx
            native = (target == LINUX and NATIVE_LINUX) or (target == DARWIN and sys.platform == 'darwin')
            if peer and native:
                subprocess.run([peer, '-std=c++26', *macros, 'bit-fields-peer.cpp', 'bit-fields.o', '-o', 'wide-fields'],
                               cwd=scope, check=True, capture_output=True, timeout=40)
                subprocess.run([scope / 'wide-fields'], cwd=scope, check=True, timeout=20)


def over_width_bit_field_diagnostics(world):
    scope = world.scope
    (scope / 'bit-field-diagnostic.cpp').write_text('struct Record { unsigned value : 40; };\n')
    for target in (LINUX, DARWIN):
        for args in ((), ('-w',), ('-Werror',), ('-Wno-bitfield-width',), ('-Wbitfield-width',),
                     ('-Werror=bitfield-width',), ('-Werror', '-Wno-error=bitfield-width'), ('-pedantic-errors',)):
            for mode in MODES:
                unavailable = mode == 'gcc' and any(arg in args for arg in (
                    '-Wbitfield-width', '-Werror=bitfield-width', '-Wno-error=bitfield-width'))
                suppressed = '-w' in args or (mode == 'clang' and '-Wno-bitfield-width' in args)
                diagnosed = mode != 'spec' and not suppressed and not unavailable
                promoted = diagnosed and '-Wno-error=bitfield-width' not in args and (
                    '-Werror' in args or '-Werror=bitfield-width' in args)
                command = [target, '-fcompat=' + mode, *args, '-fsyntax-only', 'bit-field-diagnostic.cpp']
                cold = world.cxx(command, scope, world.cold, timeout=40)
                resident = world.cxx(command, scope, world.daemon, timeout=40)
                assert cold == resident, (mode, target, args, cold, resident)
                assert resident[0] == int(unavailable or promoted), (mode, target, args, resident)
                if unavailable:
                    assert b'unsupported option' in resident[2], (mode, target, args, resident)
                elif diagnosed:
                    message = b"'Record::value' exceeds its type" if mode == 'gcc' else b'truncated to 32 bits'
                    severity = b'error: ' if promoted else b'warning: '
                    assert message in resident[2] and severity in resident[2], (mode, target, args, resident)
                else:
                    assert resident[2] == b'', (mode, target, args, resident)


def immediate_template_escalation(world):
    scope = world.scope
    (scope / 'immediate-template.cpp').write_text('''\
consteval int next(int value) { return value + 1; }
constexpr int ordinary(int value) { return value + 1; }
template<auto F> constexpr auto apply(int value) { return F(value); }
template<auto F> constexpr auto fixed(int) { return F(3); }
template<class T> constexpr int direct(T value) { return next(value); }
#if CASE == 1
static_assert(apply<next>(3) == 4);
#elif CASE == 2
auto p = &apply<next>;
#elif CASE == 3
int runtime(int value) { return apply<next>(value); }
#elif CASE == 4
auto p = &fixed<next>;
#elif CASE == 5
auto p = &apply<ordinary>;
static_assert(apply<ordinary>(3) == 4);
#elif CASE == 6
static_assert(direct(3) == 4);
#elif CASE == 7
auto p = &direct<int>;
#elif CASE == 8
int (*p)(int) = &apply<next>;
#elif CASE == 9
template<int (*F)(int)> constexpr int typed(int value) { return F(value); }
static_assert(typed<next>(3) == 4);
#elif CASE == 10
template<auto F> void ignore() {}
void use() { ignore<&next>(); }
#endif
''')
    for target in (LINUX, DARWIN):
        for case in range(1, 11):
            for mode in MODES:
                clang_argument = mode == 'clang' and case in (1, 2, 3, 4, 8, 9, 10)
                address_escape = case in (2, 7, 8) or (mode == 'gcc' and case == 4)
                command = [target, '-fcompat=' + mode, f'-DCASE={case}', '-fsyntax-only', 'immediate-template.cpp']
                cold = world.cxx(command, scope, world.cold, timeout=40)
                resident = world.cxx(command, scope, world.daemon, timeout=40)
                assert cold == resident, (mode, target, case, cold, resident)
                assert resident[0] == int(clang_argument or address_escape or case == 3), (mode, target, case, resident)
                if clang_argument or address_escape:
                    assert b'address of immediate function' in resident[2], (mode, target, case, resident)
                elif case == 3:
                    assert b'not a constant expression' in resident[2], (mode, target, case, resident)


def lambda_conversion_symbols(world, gxx):
    scope = world.scope
    (scope / 'lambda-conversion.cpp').write_text('''\
using I = int(*)(int);
using N = int(*)(int) noexcept;
inline I plain() { return [](int v) { return v; }; }
inline I generic() { return [](auto v) { return v + 1; }; }
inline I typed() { return []<class T>(T v) -> T { return v; }; }
inline I mut() { return [](auto v) mutable { return v; }; }
inline N noex() { return [](auto v) noexcept { return v; }; }
extern "C" int exercise_conversion() { return plain()(1) + generic()(2) + typed()(3) + mut()(4) + noex()(5); }
''')
    (scope / 'lambda-conversion-caller.cpp').write_text('''\
using I = int(*)(int);
extern "C" I convert(const void*) __asm__(CONVERSION);
extern "C" int exercise_conversion();
int main() { unsigned char closure = 0; return convert(&closure)(7) != 8 || exercise_conversion() != 16; }
''')
    gcc_result = 'DTcldtdeLKPKS0_0EonclIS_EscOS_fp_EES_'
    clang_result = 'DaS_'
    common = ('_ZZ5plainvENKUliE_cvPFiiEEv', '_ZZ5typedvENKUlTyT_E_cvPFS_S_EIiEEv')
    clang = shutil.which(os.environ.get('YCXX_CLANGXX', 'clang++-23'))
    for mode in MODES:
        encoding = gcc_result if mode == 'gcc' else clang_result
        generic = f'_ZZ7genericvENKUlT_E_cvPF{encoding}EIiEEv'
        mut = encoding.replace('KPKS0_', 'KPS0_')
        symbols = (*common, generic, f'_ZZ3mutvENKUlT_E_cvPF{mut}EIiEEv', f'_ZZ4noexvENKUlT_E_cvPDoF{encoding}EIiEEv')
        for target in (LINUX, DARWIN):
            command = [target, '-fcompat=' + mode, '-O0', '-c', 'lambda-conversion.cpp', '-o', 'lambda-conversion.o']
            binaries = []
            for env in (world.cold, world.daemon):
                result = world.cxx(command, scope, env, timeout=40)
                assert result == (0, b'', b''), (mode, target, result)
                binaries.append((scope / 'lambda-conversion.o').read_bytes())
            assert binaries[0] == binaries[1], (mode, target)
            names = set(binaries[0].split(b'\0'))
            prefix = '_' if target == DARWIN else ''
            if mode == 'spec':
                assert all(b'_cvP' not in name for name in names), (mode, target, names)
                continue
            for symbol in symbols:
                assert (prefix + symbol).encode() in names, (mode, target, symbol, [n for n in names if b'_cvP' in n])
            if (target == LINUX and NATIVE_LINUX) or (target == DARWIN and sys.platform == 'darwin'):
                peer = gxx if mode == 'gcc' else clang
                if not peer:
                    continue
                define = f'-DCONVERSION="{prefix}{generic}"'
                subprocess.run([peer, '-std=c++26', define, 'lambda-conversion-caller.cpp', 'lambda-conversion.o',
                                '-o', 'lambda-conversion'], cwd=scope, check=True, capture_output=True, timeout=40)
                subprocess.run([scope / 'lambda-conversion'], cwd=scope, check=True, timeout=20)


def floating_underflow_warnings(world):
    scope = world.scope
    (scope / 'literal-underflow.cpp').write_text('''\
constexpr float f = 0x1p-150f;
constexpr double d = 0x1p-1075;
constexpr long double l = 0x1p-16446L;
constexpr float zero = 0.0f;
constexpr float subnormal = 0x1p-149f;
''')
    for target in (LINUX, DARWIN):
        for mode in MODES:
            own = 'overflow' if mode == 'gcc' else 'literal-range'
            peer = 'literal-range' if mode == 'gcc' else 'overflow'
            cases = (([], mode != 'spec', False), (['-Wno-' + own], False, False),
                     (['-Wno-' + peer], mode != 'spec', False),
                     (['-Werror=' + own], mode != 'spec', mode != 'spec'),
                     (['-Werror=' + own, '-Wno-error=' + own], mode != 'spec', False))
            for options, diagnosed, rejected in cases:
                args = [target, '-fcompat=' + mode, *options, '-fsyntax-only', 'literal-underflow.cpp']
                cold, warm = (world.cxx(args, scope, env, timeout=30) for env in (world.cold, world.daemon))
                assert cold == warm and warm[0] == int(rejected), (mode, target, options, cold, warm)
                assert warm[2].count(b' to zero') == (3 if diagnosed else 0), (mode, target, options, warm[2])


def static_lambda_conversion(world, gxx):
    scope = world.scope
    (scope / 'static-lambda.cpp').write_text('''\
using P = int (*)(int);
inline P plain() { return +[](int n) static { return n + 1; }; }
inline P generic() { return [](auto n) static { return n + 2; }; }
inline P explicit_generic() { return []<class T>(T n) static { return n + 3; }; }
extern "C" P export_plain() { return plain(); }
extern "C" P export_generic() { return generic(); }
extern "C" P export_explicit_generic() { return explicit_generic(); }
''')
    (scope / 'static-caller.cpp').write_text('''\
using P = int (*)(int);
extern "C" P export_plain();
extern "C" P export_generic();
extern "C" P export_explicit_generic();
int main() {
  return export_plain()(10) != 11 || export_generic()(20) != 22 ||
         export_explicit_generic()(30) != 33;
}
''')
    (scope / 'static-lambda-invalid.cpp').write_text('''\
int invalid(int n) {
#ifdef MUTABLE
  return [](int p) mutable static { return p; }(n);
#else
  return [n]() static { return n; }();
#endif
}
''')
    clang = shutil.which(os.environ.get('YCXX_CLANGXX', 'clang++-23'))
    operators = (
        '_ZZ5plainvENUliE_clEi',
        '_ZZ7genericvENUlT_E_clIiEEDaS_',
        '_ZZ16explicit_genericvENUlTyT_E_clIiEEDaS_',
    )
    for mode in MODES:
        pattern = 'DTclonclscOS_fp_EE' if mode == 'gcc' else 'Da'
        conversions = (
            '_ZZ5plainvENKUliE_cvPFiiEEv',
            f'_ZZ7genericvENKUlT_E_cvPF{pattern}S_EIiEEv',
            f'_ZZ16explicit_genericvENKUlTyT_E_cvPF{pattern}S_EIiEEv',
        )
        for target in (LINUX, DARWIN):
            flags = [target, '-fcompat=' + mode]
            outputs = []
            for env in (world.cold, world.daemon):
                command = [*flags, '-c', 'static-lambda.cpp', '-o', 'static-lambda.o']
                result = world.cxx(command, scope, env, timeout=40)
                assert result == (0, b'', b''), (mode, target, result)
                outputs.append((scope / 'static-lambda.o').read_bytes())
            assert outputs[0] == outputs[1], (mode, target)
            prefix = '_' if target == DARWIN else ''
            names = outputs[1].split(b'\0')
            assert all((prefix + name).encode() in names for name in operators), (mode, target, names)
            assert not any(b'__invoke' in name or b'_FUN' in name for name in names), (mode, target, names)
            for name in conversions:
                assert ((prefix + name).encode() in names) == (mode != 'spec'), (mode, target, name, names)
            for options in ([], ['-DMUTABLE']):
                args = [*flags, *options, '-fsyntax-only', 'static-lambda-invalid.cpp']
                cold, warm = (world.cxx(args, scope, env, timeout=40) for env in (world.cold, world.daemon))
                assert cold == warm and warm[0] == 1 and b'static lambda' in warm[2], (mode, target, cold, warm)
            peer = clang if mode == 'clang' else gxx
            if peer and ((target == LINUX and NATIVE_LINUX) or (target == DARWIN and sys.platform == 'darwin')):
                subprocess.run([peer, '-std=c++26', 'static-caller.cpp', 'static-lambda.o', '-o', 'static-call'],
                               cwd=scope, check=True, capture_output=True, timeout=40)
                subprocess.run([scope / 'static-call'], cwd=scope, check=True, timeout=20)


def constrained_lambda_conversions(world, gxx):
    scope = world.scope
    (scope / 'constrained-conversion.cpp').write_text('''\
template<class T> concept Size = sizeof(T) >= 2;
#ifdef REJECT
using F = char (*)(char);
#else
using F = int (*)(int);
#endif
#if CASE == 1
inline F f() { return []<Size T>(T n) { return n; }; }
#elif CASE == 2
inline F f() { return [](auto n) requires Size<decltype(n)> { return n; }; }
#elif CASE == 3
inline F f() { return []<class T> requires Size<T> (T n) { return n; }; }
#elif CASE == 4
inline F f() { return []<Size T>(T n) static { return n; }; }
#elif CASE == 5
inline F f() { return [](auto n) static requires Size<decltype(n)> { return n; }; }
#else
inline F f() { return [](Size auto n) { return n; }; }
#endif
extern "C" F obtain() { return f(); }
''')
    clang = shutil.which(os.environ.get('YCXX_CLANGXX', 'clang++-23'))
    heads = ('UlTk4SizeT_E', 'UlT_E', 'UlTyQ4SizeIT_ES_E', 'UlTk4SizeT_E', 'UlT_E', 'UlT_E')
    for mode in MODES:
        peer = gxx if mode == 'gcc' else clang
        for target in (LINUX, DARWIN):
            flags = [target, '-fcompat=' + mode]
            prefix = '_' if target == DARWIN else ''
            for case, head in enumerate(heads, 1):
                trailing = case in (2, 5)
                static = case in (4, 5)
                ret = 'Da'
                if mode == 'gcc':
                    ret = 'DTclonclscOS_fp_EE' if static else 'DTcldtdeLKPKS0_0EonclIS_EscOS_fp_EE'
                suffix = 'Q4SizeIDtfp_EE' if mode == 'clang' and trailing else ''
                symbol = f'{prefix}_ZZ1fvENK{head}_cvPF{ret}S_EIiEEv{suffix}'
                command = [*flags, f'-DCASE={case}', '-O0', '-c', 'constrained-conversion.cpp', '-o', 'constrained-conversion.o']
                outputs = []
                for env in (world.cold, world.daemon):
                    result = world.cxx(command, scope, env, timeout=40)
                    assert result == (0, b'', b''), (mode, target, case, result)
                    outputs.append((scope / 'constrained-conversion.o').read_bytes())
                names = outputs[1].split(b'\0')
                assert outputs[0] == outputs[1], (mode, target, case)
                if mode == 'spec':
                    assert not any(b'_cvP' in name for name in names), (mode, target, case, names)
                else:
                    assert symbol.encode() in names, (mode, target, case, symbol, [n for n in names if b'_Z' in n])
                rejection = [*flags, f'-DCASE={case}', '-DREJECT', '-fsyntax-only', 'constrained-conversion.cpp']
                cold, warm = (world.cxx(rejection, scope, env, timeout=40) for env in (world.cold, world.daemon))
                assert cold == warm and warm[0] == 1, (mode, target, case, cold, warm)
                native = (target == LINUX and NATIVE_LINUX) or (target == DARWIN and sys.platform == 'darwin')
                if peer and native and mode != 'spec' and trailing:
                    (scope / 'constrained-conversion-caller.cpp').write_text(f'''\
using F = int (*)(int);
extern "C" F convert(const void*) asm("{symbol}");
extern "C" F obtain();
int main() {{ unsigned char closure = 0; return convert(&closure)(19) != 19 || obtain()(23) != 23; }}
''')
                    subprocess.run([peer, '-std=c++26', 'constrained-conversion-caller.cpp', 'constrained-conversion.o',
                                    '-o', 'constrained-conversion'], cwd=scope, check=True, capture_output=True, timeout=40)
                    subprocess.run([scope / 'constrained-conversion'], cwd=scope, check=True, timeout=20)


def nested_template_symbols(world, gxx):
    scope = world.scope
    declarations = '''\
template<class T> concept C = sizeof(T) > 0;
template<class T, class U = T> struct X {};
template<class T> requires C<T> int global(T n) BODY(n)
namespace N {
  template<class T> requires C<T> int named(T n) BODY(n)
  template<class T> int trailing(T n) requires C<T> BODY(n)
  template<class T> requires C<T> int both(T n) requires (sizeof(T) >= 1) BODY(n)
  template<auto V> int placeholder() BODY(V)
  template<template<class> class Q> int templ() BODY(11)
}
struct S {
  template<class T> requires C<T> static int member(T n) BODY(n)
  template<class T> static int trailing(T n) requires C<T> BODY(n)
  template<C T> static int typed(T n) BODY(n)
  template<C... T> static int pack(T... n) BODY(sizeof...(n))
};
'''
    exercise = ('global(1) + N::named(2) + N::trailing(3) + N::both(4) + S::member(5) + S::trailing(6) + '
                'S::typed(7) + S::pack(8, 9) + N::placeholder<10>() + N::templ<X>()')
    (scope / 'nested-provider.cpp').write_text(
        '#define BODY(EXPR) { return EXPR; }\n' + declarations + 'int use() { return ' + exercise + '; }\n')
    (scope / 'nested-caller.cpp').write_text(
        '#define BODY(EXPR) ;\n' + declarations + 'int main() { return (' + exercise + ') != 51; }\n')
    common = ('_Z6globalIiQ1CIT_EEiS0_', '_ZN1N8trailingIiEEiT_Q1CIS1_E', '_ZN1S8trailingIiEEiT_Q1CIS1_E')
    gcc = ('_ZN1N5namedIiEEiT_', '_ZN1N4bothIiEEiT_QgestS1_Li1E', '_ZN1S6memberIiEEiT_', '_ZN1S5typedIiEEiT_',
           '_ZN1S4packIJiiEEEiDpT_', '_ZN1N11placeholderILi10EEEiv', '_ZN1N5templI1XEEiv')
    structural = ('_ZN1N5namedIiQ1CIT_EEEiS1_', '_ZN1N4bothIiQ1CIT_EEEiS1_QgestS1_Li1E',
                  '_ZN1S6memberIiQ1CIT_EEEiS1_', '_ZN1S5typedITk1CiEEiT_', '_ZN1S4packITpTk1CJiiEEEiDpT_',
                  '_ZN1N11placeholderITnDaLi10EEEiv', '_ZN1N5templITtTyE1XEEiv')
    clang = shutil.which(os.environ.get('YCXX_CLANGXX', 'clang++-23'))
    for mode in MODES:
        symbols = (*common, *(gcc if mode == 'gcc' else structural))
        for target in (LINUX, DARWIN):
            command = [target, '-fcompat=' + mode, '-c', 'nested-provider.cpp', '-o', 'nested-provider.o']
            outputs = []
            for env in (world.cold, world.daemon):
                result = world.cxx(command, scope, env, timeout=40)
                assert result == (0, b'', b''), (mode, target, result)
                outputs.append((scope / 'nested-provider.o').read_bytes())
            assert outputs[0] == outputs[1], (mode, target)
            names = outputs[1].split(b'\0')
            prefix = '_' if target == DARWIN else ''
            for symbol in symbols:
                assert (prefix + symbol).encode() in names, (mode, target, symbol, names)
            peer = gxx if mode == 'gcc' else clang
            if peer and ((target == LINUX and NATIVE_LINUX) or (target == DARWIN and sys.platform == 'darwin')):
                abi = ['-fabi-compat-version=0'] if mode == 'gcc' else []
                subprocess.run([peer, '-std=c++26', *abi, 'nested-caller.cpp', 'nested-provider.o', '-o', 'nested-call'],
                               cwd=scope, check=True, capture_output=True, timeout=40)
                subprocess.run([scope / 'nested-call'], cwd=scope, check=True, timeout=20)


def lambda_constraint_expression_symbols(world, gxx):
    scope = world.scope
    (scope / 'constraint-lambda.cpp').write_text('''\
#ifdef ORDER
int ordinary() { return [] { return 1; }(); }
template<class T> requires ([] { return true; }()) int unused(T n) { return n; }
template<class T> requires ([] { return true; }()) int first(T n) { return n; }
namespace N { template<class T> int second(T n) requires ([] { return true; }()) { return n; } }
template<class T> requires requires { [] { return 1; }(); } int third(T n) { return n; }
int use() {
  int a = third(3);
  int b = first(1);
  int c = N::second(2);
  return a + b + c;
}
#else
template<class T> requires ([](T n) { return n == 3; }(T(3))) int f(T n) { return n; }
int use() { return f(7); }
#endif
''')
    clang = shutil.which(os.environ.get('YCXX_CLANGXX', 'clang++-23'))
    for mode in MODES:
        signatures = ('_Z1fIiQclL3$_0EcvT_Li3EEEiS1_', '_Z5firstIiQclL3$_2EEEiT_',
                      '_Z5thirdIiQrqXclL3$_1EEEEiT_', '_ZN1N6secondIiEEiT_QclLNS_3$_3EEE') if mode == 'clang' else (
                      '_Z1fIiQclLUlT_E_EcvS0_Li3EEEiS0_', '_Z5firstIiQclLUlvE0_EEEiT_',
                      '_Z5thirdIiQrqXclLUlvE2_EEEEiT_', '_ZN1N6secondIiEEiT_QclLNS_UlvE1_EEE')
        for target in (LINUX, DARWIN):
            prefix = '_' if target == DARWIN else ''
            for variant, names in (([], signatures[:1]), (['-DORDER'], signatures[1:])):
                command = [target, '-fcompat=' + mode, *variant, '-c', 'constraint-lambda.cpp', '-o', 'constraint-lambda.o']
                outputs = []
                for env in (world.cold, world.daemon):
                    result = world.cxx(command, scope, env, timeout=40)
                    assert result == (0, b'', b''), (mode, target, variant, result)
                    outputs.append((scope / 'constraint-lambda.o').read_bytes())
                assert outputs[0] == outputs[1], (mode, target, variant)
                exposed = outputs[1].split(b'\0')
                for name in names:
                    assert (prefix + name).encode() in exposed, (mode, target, variant, name, exposed)
                peer = clang if mode == 'clang' else gxx
                native = (target == LINUX and NATIVE_LINUX) or (target == DARWIN and sys.platform == 'darwin')
                if peer and native:
                    symbol = prefix + names[0]
                    (scope / 'constraint-lambda-caller.cpp').write_text(f'''\
extern "C" int call(int) asm("{symbol}");
int main() {{ return call(19) != 19; }}
''')
                    subprocess.run([peer, '-std=c++26', 'constraint-lambda-caller.cpp', 'constraint-lambda.o',
                                    '-o', 'constraint-lambda'], cwd=scope, check=True, capture_output=True, timeout=40)
                    subprocess.run([scope / 'constraint-lambda'], cwd=scope, check=True, timeout=20)


def implicit_object_constraint_symbols(world, gxx):
    scope = world.scope
    (scope / 'this-constraint.cpp').write_text('''\
#if CASE == 1
struct S { template<class T> int f(T n) const requires (sizeof(this) >= 1) { return n; } };
#elif CASE == 2
struct S { template<class T> int f(T n) const requires (sizeof(*this) >= 1) { return n; } };
#elif CASE == 3
template<class U> struct Record {
  template<class T> int f(T n) const requires (sizeof(*this) >= 1) { return n; }
};
using S = Record<int>;
#elif CASE == 4
struct S {
  template<class T> int h() const { return 1; }
  template<class T> int f(T n) const requires requires { this->template h<T>(); } { return n; }
};
#elif CASE == 5
struct S { template<class T> int f(T n) const requires (sizeof(this) == 1) { return n; } };
#elif CASE == 6
struct S {
  template<class T> requires (sizeof(T) > 8) int h() const { return 1; }
  template<class T> int f(T n) const requires requires { this->template h<T>(); } { return n; }
};
#endif
int use() { S x; return x.f(7); }
''')
    clang = shutil.which(os.environ.get('YCXX_CLANGXX', 'clang++-23'))
    for mode in MODES:
        value = 'szfpT' if mode == 'gcc' else 'Lm8E'
        symbols = {1: f'_ZNK1S1fIiEEiT_Qge{value}Li1E', 3: '_ZNK6RecordIiE1fIiEEiT_QgeszdefpTLi1E',
                   4: '_ZNK1S1fIiEEiT_QrqXclptfpT1hIS1_EEE'}
        for target in (LINUX, DARWIN):
            prefix = '_' if target == DARWIN else ''
            for case in range(1, 7):
                flags = [target, '-fcompat=' + mode, f'-DCASE={case}']
                if case not in symbols:
                    rejection = [*flags, '-fsyntax-only', 'this-constraint.cpp']
                    cold, warm = (world.cxx(rejection, scope, env, timeout=40) for env in (world.cold, world.daemon))
                    assert cold == warm and warm[0] == 1, (mode, target, case, cold, warm)
                    diagnostic = b'incomplete' if case == 2 else b'no matching function'
                    assert diagnostic in warm[2], (mode, target, case, warm)
                    continue
                command = [*flags, '-c', 'this-constraint.cpp', '-o', 'this-constraint.o']
                binaries = []
                for env in (world.cold, world.daemon):
                    result = world.cxx(command, scope, env, timeout=40)
                    assert result == (0, b'', b''), (mode, target, case, result)
                    binaries.append((scope / 'this-constraint.o').read_bytes())
                assert binaries[0] == binaries[1], (mode, target, case)
                symbol = prefix + symbols[case]
                assert symbol.encode() in binaries[1].split(b'\0'), (mode, target, case, symbol)
                peer = clang if mode == 'clang' else gxx
                native = (target == LINUX and NATIVE_LINUX) or (target == DARWIN and sys.platform == 'darwin')
                if peer and native:
                    (scope / 'this-constraint-caller.cpp').write_text(f'''\
struct Empty {{}};
extern "C" int call(const Empty&, int) asm("{symbol}");
int main() {{ return call(Empty{{}}, 19) != 19; }}
''')
                    subprocess.run([peer, '-std=c++26', 'this-constraint-caller.cpp', 'this-constraint.o',
                                    '-o', 'this-constraint'], cwd=scope, check=True, capture_output=True, timeout=40)
                    subprocess.run([scope / 'this-constraint'], cwd=scope, check=True, timeout=20)


def remaining_constraint_symbols(world, gxx):
    scope = world.scope
    (scope / 'constraint-layout.cpp').write_text(r'''
#if CASE == 0
#define C (sizeof(int) >= 1 && sizeof(T) > 0)
#elif CASE == 1
#define C (alignof(int) >= 1 && sizeof(T) > 0)
#elif CASE == 2
#define C (sizeof("a\0b\0") >= 1 && sizeof(T) > 0)
#else
#define C (sizeof("\xff") >= 1 && sizeof(T) > 0)
#endif
template<class T> requires C int f(T) { return 7; }
int call() { return f(7); }
''')
    (scope / 'constraint-cast.cpp').write_text('''
struct Data { int member; };
#if CASE == 0
#define C requires(T* p) { reinterpret_cast<unsigned long>(p); }
#elif CASE == 1
#define C requires(T* p) { (void*)p; }
#elif CASE == 2
#define C requires(Data* p) { dynamic_cast<Data*>(p); sizeof(T); }
#elif CASE == 3
#define C requires(T obj, int T::*mem) { obj.*mem; }
#elif CASE == 4
#define C requires(T* obj, int T::*mem) { obj->*mem; }
#elif CASE == 5
#define C requires(T t) { __builtin_bit_cast(T, t); }
#else
#define C requires { __builtin_offsetof(T, member); }
#endif
template<class T> requires C int f(T*) { return 7; }
int call() { return f((Data*)0); }
''')
    clang = shutil.which(os.environ.get('YCXX_CLANGXX', 'clang++-23'))
    for mode in MODES:
        first, second = ('fL0p_', 'fL0p0_') if mode == 'clang' else ('fp_', 'fp0_')
        clauses = (f'rQPT__Xrcm{first}E', f'rQPT__XcvPv{first}E', f'rQPS0__XdcS1_{first}XstT_E',
                   f'rQT_MS1_i_Xds{first}{second}E', f'rQPT_MS1_i_Xpm{first}{second}E')
        parameters = ('S2_', 'S2_', 'PS2_', 'PS1_', 'S2_')
        casts = tuple(f'_Z1fI4DataQ{clause}Ei{parameter}' for clause, parameter in zip(clauses, parameters))
        folded = ('Lm4E', 'Lm4E', 'Lm5E', 'Lm2E')
        retained = ('sti', 'Lm4E', 'sztlA5_KcLS0_97ELS0_0ELS0_98EE', 'sztlA2_KcLS0_255EE')
        layouts = tuple(
            f'_Z1fIiQaage{operand}Li1EgtstT_Li0EEiS{2 if mode == "gcc" and case >= 2 else 0}_'
            for case, operand in enumerate(retained if mode == 'gcc' else folded))
        peer = gxx if mode == 'gcc' else clang
        for target in (LINUX, DARWIN):
            prefix = '_' if target == DARWIN else ''
            flags = [target, '-fcompat=' + mode]
            native = (target == LINUX and NATIVE_LINUX) or (target == DARWIN and sys.platform == 'darwin')
            for source, symbols, mixed_case, typ, argument in (
                ('constraint-layout.cpp', layouts, 2, 'int', '7'),
                ('constraint-cast.cpp', casts, 3, 'Data*', 'nullptr'),
            ):
                for case, name in enumerate(symbols):
                    name = prefix + name
                    command = [*flags, f'-DCASE={case}', '-c', source, '-o', 'remaining-constraint.o']
                    outputs = []
                    for env in (world.cold, world.daemon):
                        result = world.cxx(command, scope, env, timeout=40)
                        assert result == (0, b'', b''), (mode, target, source, case, result)
                        outputs.append((scope / 'remaining-constraint.o').read_bytes())
                    names = outputs[1].split(b'\0')
                    assert outputs[0] == outputs[1] and name.encode() in names, (
                        mode, target, source, case, name, [n for n in names if b'_Z1f' in n])
                    if peer and native and case == mixed_case:
                        (scope / 'remaining-constraint-caller.cpp').write_text(f'''
struct Data {{ int member; }};
extern "C" int f({typ}) asm("{name}");
int main() {{ return f({argument}) != 7; }}
''')
                        subprocess.run([peer, '-std=c++26', 'remaining-constraint-caller.cpp', 'remaining-constraint.o',
                                        '-o', 'remaining-constraint'], cwd=scope, check=True, capture_output=True, timeout=40)
                        subprocess.run([scope / 'remaining-constraint'], cwd=scope, check=True, capture_output=True, timeout=20)
            for case, builtin in enumerate((b'__builtin_bit_cast', b'__builtin_offsetof'), 5):
                source = [*flags, f'-DCASE={case}', 'constraint-cast.cpp']
                syntax = [*source, '-fsyntax-only']
                cold, warm = (world.cxx(syntax, scope, env, timeout=40) for env in (world.cold, world.daemon))
                assert cold == warm == (0, b'', b''), (mode, target, builtin, cold, warm)
                codegen = [*source, '-c', '-o', 'remaining-constraint.o']
                cold, warm = (world.cxx(codegen, scope, env, timeout=40) for env in (world.cold, world.daemon))
                assert cold == warm and warm[0] == (0 if mode == 'spec' else 1), (mode, target, builtin, cold, warm)
                if mode == 'spec':
                    assert b'__ycxx_constraint_' in (scope / 'remaining-constraint.o').read_bytes()
                else:
                    assert builtin in warm[2] and b'written constraint' in warm[2], warm[2]


def expression_alignof(world, gxx):
    scope = world.scope
    (scope / 'expression-alignof.cpp').write_text('''
struct Record { alignas(32) char aligned; int field : 3; };
Record record;
int source;
#if CASE == 0
static_assert(alignof(int) == 4);
#elif CASE == 1
static_assert(alignof(source) == 4 && alignof source == 4);
static_assert(alignof(record.aligned) == 32);
#elif CASE == 2
static_assert(alignof(record.field) == 4);
#else
template<class T> requires (alignof(source) > 0 && sizeof(T) > 0)
int g(T n) { return n; }
int call() { return g(7); }
#endif
''')
    clang = shutil.which(os.environ.get('YCXX_CLANGXX', 'clang++-23'))
    for mode in MODES:
        for target in (LINUX, DARWIN):
            flags = [target, '-fcompat=' + mode]
            controls = {
                'spec': [((), 1, b'type specifier'), (('-w',), 1, b'type specifier')],
                'gcc': [((), 0, b''), (('-Werror',), 0, b''), (('-Wpedantic',), 0, b'warning: ISO C++'),
                        (('-Werror=pedantic',), 1, b'error: ISO C++'),
                        (('-pedantic-errors',), 1, b'error: ISO C++'),
                        (('-pedantic-errors', '-Wno-pedantic'), 0, b'')],
                'clang': [((), 0, b'warning: alignof'), (('-Werror',), 1, b'error: alignof'),
                          (('-pedantic-errors',), 1, b'error: alignof'),
                          (('-Wno-gnu-alignof-expression',), 0, b''),
                          (('-pedantic-errors', '-Wno-gnu-alignof-expression'), 0, b'')],
            }
            for case, options, status, text in (
                (0, (), 0, b''),
                (2, (), 1, b'type specifier' if mode == 'spec' else b'bit-field'),
                *((1, options, status, text) for options, status, text in controls[mode]),
            ):
                args = [*flags, *options, f'-DCASE={case}', '-fsyntax-only', 'expression-alignof.cpp']
                cold, warm = (world.cxx(args, scope, env, timeout=40) for env in (world.cold, world.daemon))
                assert cold == warm and warm[0] == status, (mode, target, case, options, cold, warm)
                assert (text in warm[2]) if text else not warm[2], (mode, target, options, warm)
            if mode == 'spec':
                continue
            args = [*flags, '-w', '-DCASE=3', '-c', 'expression-alignof.cpp', '-o', 'expression-alignof.o']
            outputs = []
            for env in (world.cold, world.daemon):
                result = world.cxx(args, scope, env, timeout=40)
                assert result == (0, b'', b''), (mode, target, result)
                outputs.append((scope / 'expression-alignof.o').read_bytes())
            prefix = '_' if target == DARWIN else ''
            operand = 'azL_Z6sourceE' if mode == 'gcc' else 'Lm4E'
            name = f'{prefix}_Z1gIiQaagt{operand}Li0EgtstT_Li0EEiS0_'
            names = outputs[1].split(b'\0')
            assert outputs[0] == outputs[1] and name.encode() in names, (
                mode, target, name, [n for n in names if b'_Z1g' in n])
            peer = gxx if mode == 'gcc' else clang
            native = (target == LINUX and NATIVE_LINUX) or (target == DARWIN and sys.platform == 'darwin')
            if peer and native:
                (scope / 'expression-alignof-caller.cpp').write_text(f'''
extern "C" int g(int) asm("{name}");
int main() {{ return g(7) != 7; }}
''')
                subprocess.run([peer, '-std=c++26', 'expression-alignof-caller.cpp', 'expression-alignof.o',
                                '-o', 'expression-alignof'], cwd=scope, check=True, capture_output=True, timeout=40)
                subprocess.run([scope / 'expression-alignof'], cwd=scope, check=True, capture_output=True, timeout=20)


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
        private_closure_symbols(world, gxx)
        generic_invoker_symbols(world, gxx)
        dependent_variable_spellings(world, gxx)
        static_lambda_conversion(world, gxx)
        constrained_lambda_conversions(world, gxx)
        nested_template_symbols(world, gxx)
        lambda_constraint_expression_symbols(world, gxx)
        implicit_object_constraint_symbols(world, gxx)
        remaining_constraint_symbols(world, gxx)
        expression_alignof(world, gxx)
        lambda_conversion_symbols(world, gxx)
        floating_underflow_warnings(world)
        # PARITY: by design: the parameter difference here is the SysV AMD64 MEMORY class.
        if NATIVE_LINUX:
            mixed_class_calls(world, gxx)
            mixed_large_class_calls(world, gxx)
            # PARITY: by design: native array new is currently available on Linux x86-64.
            wide_array_allocations(world, gxx)
        mixed_darwin_class_calls(world, gxx)
        mixed_class_alignment(world, gxx)
        mixed_tail_padding(world, gxx)
        overlapping_member_layout(world, gxx)
        over_width_bit_fields(world, gxx)
        over_width_bit_field_diagnostics(world)
        late_defaulted_special_members(world, gxx)
        mixed_constraints(world, gxx)
        function_language_linkage(world, gxx)
        mixed_abstract_result(world, gxx)
        wide_enumerations(world, gxx)
        empty_union(world, gxx)
        pack_index_symbols(world, gxx)
        bound_pack_index_symbols(world, gxx)
        operator_constraints(world, gxx)
        requires_expression_symbols(world, gxx)
        fold_constraint_symbols(world, gxx)
        fold_constraint_ordering(world)
        fold_constraint_satisfaction(world)
        binary_fold_constraints(world)
        fold_constraint_substitution_failures(world)
        constrained_parameter_packs(world, gxx)
        concept_template_parameters(world)
        constraint_expression_symbols(world, gxx)
        constrained_lambda_members(world, gxx)
        written_trait_constraint_symbols(world, gxx)
        written_allocation_constraint_symbols(world, gxx)
        template_argument_prefixes(world, gxx)
        placeholder_template_prefixes(world, gxx)
        mixed_source_location(world, gxx)
        concrete_transform_signatures(world)
        immediate_template_escalation(world)
        compile_database(world)
        world.same_daemon('compatibility modes', pid)
    finally:
        world.close()
    return bool(failures)


if __name__ == '__main__':
    sys.exit(main())
