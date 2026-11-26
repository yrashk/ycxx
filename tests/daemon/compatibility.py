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
    (world.scope / 'dependent-template-qualifier.cpp').write_text(
        'template<class T> using R = typename T::M<int>::type;\n')
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
        invoke(*flags, '-fsyntax-only', 'line.cpp')
        invoke(*flags, '-fsyntax-only', 'descriptor.cpp')
        invoke(*flags, '-fsyntax-only', 'immediate-escape.cpp')
        for case in ('GLOBAL_POINTER', 'GLOBAL_REFERENCE', 'LOCAL_POINTER', 'LOCAL_REFERENCE'):
            invoke(*flags, f'-D{case}', '-fsyntax-only', 'immediate-escape.cpp', status=mode != 'spec')
        invoke(*flags, '-DEXPLICIT_TEMPLATE', '-fsyntax-only', 'dependent-template.cpp')
        invoke(*flags, '-fsyntax-only', 'dependent-template.cpp', status=mode == 'clang')
        invoke(*flags, '-DTEMPLATE_ARGUMENT', '-fsyntax-only', 'dependent-template.cpp', status=1)
        if mode != 'gcc':
            invoke(*flags, '-fsyntax-only', 'dependent-template-qualifier.cpp', status=1)
        invoke(*flags, '-fsyntax-only', 'alias-identity.cpp')
        invoke(*flags, '-fsyntax-only', 'memchr.cpp', status=mode == 'gcc')
        invoke(*flags, '-fsyntax-only', 'memcmp.cpp', status=mode != 'clang')
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
            invoke(target, *flags, '-fsyntax-only', 'member-alignment.cpp')
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
        mixed_constraints(world, gxx)
        compile_database(world)
        world.same_daemon('compatibility modes', pid)
    finally:
        world.close()
    return bool(failures)


if __name__ == '__main__':
    sys.exit(main())
