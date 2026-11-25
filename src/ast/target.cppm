// What the front end needs to know of a target: sizes and alignments of the fundamental types,
// the signedness of `char`, the underlying types of the character types and of `size_t`, and the
// floating-point formats ([basic.fundamental], [basic.extended.fp]). One constant table per
// target; AArch64 Darwin is the first (DECISIONS §1.3).
export module ycxx.ast.target;

import std;
import ycxx.ast.handles;
import ycxx.ast.types;

export namespace ycxx::ast {

// The value sets of the floating-point types, for their conversion ranks ([conv.rank]).
enum class FloatFormat : u8 { none, binary16, bfloat16, binary32, binary64, x87_extended, binary128 };

// Precision and exponent bits: one format's values are a subset of another's when both are no
// larger.
struct FloatShape {
  u8 precision;
  u8 exponent;
};

constexpr FloatShape shape_of(FloatFormat f) {
  switch (f) {
    case FloatFormat::none: return {0, 0};
    case FloatFormat::binary16: return {11, 5};
    case FloatFormat::bfloat16: return {8, 8};
    case FloatFormat::binary32: return {24, 8};
    case FloatFormat::binary64: return {53, 11};
    case FloatFormat::x87_extended: return {64, 15};
    case FloatFormat::binary128: return {113, 15};
  }
  std::unreachable();
}

// A fundamental type's layout. Size 0: the type has no objects (`void`, the markers) or the target
// lacks it.
struct BuiltinLayout {
  u8 size = 0;
  u8 align = 0;
  FloatFormat format = FloatFormat::none;
};

// What `__builtin_va_list` names (docs/EXTENSIONS.md): `char*` in Apple's arm64 ABI; the System
// V x86-64 ABI's `__va_list_tag[1]`.
enum class VaList : u8 { char_pointer, sysv_x86_64 };

// When a class is trivially copyable for the purposes of calls, so that it passes in registers
// (DECISIONS §1.6). `itanium`: [class.temporary]/3 and the Itanium C++ ABI, as GCC and upstream
// Clang have it, the default everywhere. `apple`: as Apple Clang has it on arm64 Darwin, a trivial
// destructor and a copy constructor trivial or deleted, whatever the move constructors; selected
// by `-fclass-call-abi=apple` on a target that offers it.
enum class CallTriviality : u8 { itanium, apple };

enum class Architecture : u8 { aarch64, x86_64 };
enum class OperatingSystem : u8 { darwin, linux_ };
enum class ObjectFormat : u8 { macho, elf };

// Native operations the compiler implements for a target. Their source and ABI
// preconditions are checked by each operation's own analysis.
enum class NativeOperation : std::uint64_t {
  derived_virtual_destructor = std::uint64_t{1} << 0,
  source_location_descriptor = std::uint64_t{1} << 5,
  peer_typed_dynamic_cast = std::uint64_t{1} << 1,
  namespace_hidden_asm = std::uint64_t{1} << 2,
  breakpoint_statement_asm = std::uint64_t{1} << 3,
};

enum class BreakpointInstruction : u8 { x86_int3 };

struct Target {
  std::string_view triple;
  Architecture architecture;
  OperatingSystem operating_system;
  ObjectFormat object_format;
  std::array<BuiltinLayout, builtin_count> layout;
  u8 pointer_size;
  u8 pointer_align;
  bool char_signed;
  Builtin wchar_underlying;
  Builtin char16_underlying;  // uint_least16_t
  Builtin char32_underlying;  // uint_least32_t
  Builtin size_type;
  Builtin ptrdiff_type;
  std::array<Builtin, 4> exact_width_int_types;  // int8_t, int16_t, int32_t, int64_t
  Builtin intmax_type;
  Builtin intptr_type;
  Builtin wint_type;
  Builtin sig_atomic_type;
  std::string_view multiarch;  // the C library directory within usr/include, when there is one
  VaList va_list;
  CallTriviality call_triviality;  // the default
  bool apple_calls;                 // whether `-fclass-call-abi=apple` is offered
  std::uint64_t native_operations = 0;
  // Whether all code for the target is position independent, whatever the options say: `__PIC__`
  // and `__pic__` are then always 2, as Clang has them on arm64 Darwin.
  bool always_pic = false;

  constexpr BuiltinLayout operator[](Builtin b) const { return layout[std::to_underlying(b)]; }
  constexpr bool has(Builtin b) const { return (*this)[b].size != 0; }
  constexpr bool supports(NativeOperation operation) const {
    return (native_operations & std::to_underlying(operation)) != 0;
  }

  constexpr std::optional<BreakpointInstruction> breakpoint_statement_asm(std::span<const std::uint32_t> units) const {
    if (!supports(NativeOperation::breakpoint_statement_asm) || architecture != Architecture::x86_64)
      return std::nullopt;
    constexpr std::uint32_t int3[] = {'i', 'n', 't', '3', 0};
    if (std::ranges::equal(units, int3)) return BreakpointInstruction::x86_int3;
    return std::nullopt;
  }
};

// Itanium C++ ABI §2.3: a data member pointer is a ptrdiff_t; on both supported
// LP64 targets a member function pointer is a code pointer and a ptrdiff_t adjustment.
constexpr BuiltinLayout member_pointer_layout(const Target& t, bool function) {
  BuiltinLayout offset = t[t.ptrdiff_type];
  if (!function) return offset;
  return {u8(t.pointer_size + offset.size), std::max(t.pointer_align, offset.align)};
}

// A class's layout (Itanium C++ ABI §2.4, [class.mem], [class.derived]). The one rule sema
// (`sizeof`, #194) and the back end (`layout`) both
// read, from what each component's own layout says of it (`Placed`):
// - the virtual table pointer of a polymorphic root with no primary base, at offset 0;
// - the non-virtual direct bases, in declaration order, then the non-static data members;
// - an empty base at offset 0, or else where a non-empty one would go;
// - a non-empty component at the class's data size so far (`dsize`, which leaves out the tail
//   padding of a base or class that is not POD for layout), rounded up to its alignment;
// - in each case moved on by its alignment while it would put two subobjects of one empty class
//   type at one offset ([intro.object]/9);
// - in a union, every member at offset 0 ([class.union]/2);
// - the size rounded up to a non-zero multiple of the alignment.
// "POD for layout" is Clang's: trivial and standard-layout ([class.prop]); GCC uses C++03's,
// and differs for a class whose data members are all private.
struct Placed {
  std::uint64_t size = 0;   // sizeof
  std::uint64_t align = 1;  // alignof
  std::uint64_t nvsize = 0;  // as a base: without tail padding unless POD for layout
  std::uint64_t nvalign = 1;
  bool empty = false;  // an empty class ([class.prop], Itanium §1.1)
  // Its subobjects of empty class type, by class entity and offset, itself included.
  std::vector<std::pair<std::uint32_t, std::uint64_t>> empties;
  u8 bit_width = 0;  // a bit-field has a width within its declared type's allocation unit
};
// A scalar, pointer or reference member's.
constexpr Placed scalar_placed(std::uint64_t size, std::uint64_t align) { return {size, align, size, align, false, {}}; }
constexpr Placed array_placed(const Placed& e, std::uint64_t n) {
  Placed out{e.size * n, e.align, e.size * n, e.align, false, {}};
  for (std::uint64_t i = 0; i < n && !e.empties.empty(); ++i)
    for (auto [t, at] : e.empties) out.empties.push_back({t, at + i * e.size});
  return out;
}
// A bit-field's storage within the bytes beginning at its member offset (little endian).
struct BitField {
  u8 shift = 0;
  u8 width = 0;
  constexpr explicit operator bool() const { return width != 0; }
  constexpr u32 bytes() const { return (u32(shift) + width + 7) / 8; }
  constexpr u32 mask() const { return u32(((std::uint64_t{1} << width) - 1) << shift); }
};

struct RecordLayout {
  std::uint64_t size = 1;
  std::uint64_t align = 1;
  std::uint64_t dsize = 0;
  std::uint64_t nvsize = 0;
  std::vector<std::uint64_t> base_offsets;  // the direct bases', in declaration order
  std::vector<std::uint64_t> offsets;       // the members', in declaration order
  std::vector<BitField> bit_fields;         // parallel to offsets, empty width for ordinary members
  Placed placed;                            // what it says of the class as a component
};
constexpr RecordLayout record_layout(std::span<const Placed> bases, std::span<const Placed> fields, bool union_, bool pod,
                                     std::uint32_t self, const Target* polymorphic_root = nullptr, u8 explicit_alignment = 0) {
  RecordLayout out;
  std::uint64_t size = polymorphic_root ? polymorphic_root->pointer_size : 0;
  std::uint64_t align = polymorphic_root ? polymorphic_root->pointer_align : 1;
  std::uint64_t dsize = size;
  std::vector<std::pair<std::uint32_t, std::uint64_t>> empties;
  auto conflicts = [&](const Placed& d, std::uint64_t at) {
    for (auto [t, x] : d.empties)
      for (auto [u, y] : empties)
        if (t == u && at + x == y) return true;
    return false;
  };
  auto place = [&](const Placed& d, bool base) {
    std::uint64_t a = base ? d.nvalign : d.align;
    std::uint64_t at = 0;
    if (!(base && d.empty)) at = (dsize + a - 1) / a * a;
    if (base && d.empty && conflicts(d, 0)) at = (dsize + a - 1) / a * a;
    while (conflicts(d, at)) at += a;
    for (auto [t, x] : d.empties) empties.push_back({t, at + x});
    align = std::max(align, a);
    if (base && d.empty) {
      size = std::max(size, at + d.size);
    } else {
      std::uint64_t n = base ? d.nvsize : d.size;
      size = std::max(size, at + n);
      dsize = at + n;
    }
    return at;
  };
  for (const Placed& b : bases) out.base_offsets.push_back(place(b, true));
  std::uint64_t next_bit = dsize * 8;
  for (const Placed& f : fields) {
    if (f.bit_width != 0) {
      // Itanium §2.4 and the target C psABI: keep a bit-field in one aligned allocation unit of
      // its declared type. An ordinary member can start at the first whole byte after it.
      std::uint64_t alignment = f.align * 8;
      if (next_bit % alignment + f.bit_width > f.size * 8)
        next_bit = (next_bit + alignment - 1) / alignment * alignment;
      out.offsets.push_back(next_bit / 8);
      out.bit_fields.push_back({u8(next_bit % 8), f.bit_width});
      next_bit += f.bit_width;
      dsize = (next_bit + 7) / 8;
      size = std::max(size, dsize);
      align = std::max(align, f.align);
      continue;
    }
    out.bit_fields.push_back({});
    if (union_) {
      out.offsets.push_back(0);
      for (auto [t, x] : f.empties) empties.push_back({t, x});
      size = std::max(size, f.size);
      dsize = std::max(dsize, f.size);
      align = std::max(align, f.align);
      continue;
    }
    out.offsets.push_back(place(f, false));
    next_bit = dsize * 8;
  }
  bool empty = !polymorphic_root && fields.empty() && std::ranges::all_of(bases, &Placed::empty);
  std::uint64_t nvsize = size;
  align = std::max(align, std::uint64_t(explicit_alignment));
  out.align = align;
  out.size = size == 0 ? align : (size + align - 1) / align * align;
  out.dsize = pod ? out.size : dsize;
  out.nvsize = pod ? out.size : nvsize;
  if (empty) out.dsize = out.nvsize = 0;  // nothing a copy writes, nothing a member cannot share
  out.placed = {out.size, out.align, out.nvsize, out.align, empty, std::move(empties)};
  if (empty) out.placed.empties.insert(out.placed.empties.begin(), {self, 0});
  return out;
}

namespace detail {

struct LayoutEntry {
  Builtin b;
  BuiltinLayout l;
};

consteval std::array<BuiltinLayout, builtin_count> layouts(std::initializer_list<LayoutEntry> es) {
  std::array<BuiltinLayout, builtin_count> r{};
  for (auto e : es) r[std::to_underlying(e.b)] = e.l;
  return r;
}

// The types whose layout is the same on every target ycxx plans for (LP64).
consteval std::array<BuiltinLayout, builtin_count> lp64(std::initializer_list<LayoutEntry> rest) {
  using enum Builtin;
  constexpr auto binary16 = FloatFormat::binary16, binary32 = FloatFormat::binary32,
                 binary64 = FloatFormat::binary64, brain16 = FloatFormat::bfloat16;
  auto r = layouts({
      {nullptr_t, {8, 8}},
      {bool_, {1, 1}},
      {char_, {1, 1}}, {signed_char, {1, 1}}, {unsigned_char, {1, 1}},
      {wchar_t_, {4, 4}}, {char8_t_, {1, 1}}, {char16_t_, {2, 2}}, {char32_t_, {4, 4}},
      {short_, {2, 2}}, {unsigned_short, {2, 2}}, {int_, {4, 4}}, {unsigned_int, {4, 4}},
      {long_, {8, 8}}, {unsigned_long, {8, 8}}, {long_long, {8, 8}}, {unsigned_long_long, {8, 8}},
      // AAPCS64 §5.1 (quad-word integers) and the System V AMD64 psABI §3.1.2 (__int128).
      {int128, {16, 16}}, {unsigned_int128, {16, 16}},
      {float_, {4, 4, binary32}}, {double_, {8, 8, binary64}},
      {float16, {2, 2, binary16}}, {float32, {4, 4, binary32}}, {float64, {8, 8, binary64}},
      {bfloat16, {2, 2, brain16}},
      {meta_info, {8, 8}},
  });
  for (auto e : rest) r[std::to_underlying(e.b)] = e.l;
  return r;
}

}  // namespace detail

// arm64-apple-darwin: `char` is signed and `long double` is `double` (Apple's ARM64 ABI); no
// binary128.
inline constexpr Target aarch64_darwin{
    .triple = "arm64-apple-darwin",
    .architecture = Architecture::aarch64,
    .operating_system = OperatingSystem::darwin,
    .object_format = ObjectFormat::macho,
    .layout = detail::lp64({{Builtin::long_double, {8, 8, FloatFormat::binary64}}}),
    .pointer_size = 8,
    .pointer_align = 8,
    .char_signed = true,
    .wchar_underlying = Builtin::int_,
    .char16_underlying = Builtin::unsigned_short,
    .char32_underlying = Builtin::unsigned_int,
    .size_type = Builtin::unsigned_long,
    .ptrdiff_type = Builtin::long_,
    .exact_width_int_types = {Builtin::signed_char, Builtin::short_, Builtin::int_, Builtin::long_long},
    .intmax_type = Builtin::long_,
    .intptr_type = Builtin::long_,
    .wint_type = Builtin::int_,
    .sig_atomic_type = Builtin::int_,
    .multiarch = {},
    .va_list = VaList::char_pointer,
    .call_triviality = CallTriviality::itanium,
    .apple_calls = true,
    .always_pic = true,
};

// x86_64-linux-gnu (System V): `long double` is the x87 80-bit format in 16 bytes.
inline constexpr Target x86_64_linux{
    .triple = "x86_64-pc-linux-gnu",
    .architecture = Architecture::x86_64,
    .operating_system = OperatingSystem::linux_,
    .object_format = ObjectFormat::elf,
    .layout = detail::lp64({{Builtin::long_double, {16, 16, FloatFormat::x87_extended}},
                            {Builtin::float128, {16, 16, FloatFormat::binary128}}}),
    .pointer_size = 8,
    .pointer_align = 8,
    .char_signed = true,
    .wchar_underlying = Builtin::int_,
    .char16_underlying = Builtin::unsigned_short,
    .char32_underlying = Builtin::unsigned_int,
    .size_type = Builtin::unsigned_long,
    .ptrdiff_type = Builtin::long_,
    .exact_width_int_types = {Builtin::signed_char, Builtin::short_, Builtin::int_, Builtin::long_},
    .intmax_type = Builtin::long_,
    .intptr_type = Builtin::long_,
    .wint_type = Builtin::unsigned_int,
    .sig_atomic_type = Builtin::int_,
    .multiarch = "x86_64-linux-gnu",
    .va_list = VaList::sysv_x86_64,
    .call_triviality = CallTriviality::itanium,
    .apple_calls = false,
    .native_operations = std::to_underlying(NativeOperation::derived_virtual_destructor) |
                         std::to_underlying(NativeOperation::peer_typed_dynamic_cast) |
                         std::to_underlying(NativeOperation::namespace_hidden_asm) |
                         std::to_underlying(NativeOperation::breakpoint_statement_asm) |
                         std::to_underlying(NativeOperation::source_location_descriptor),
};

constexpr const Target* target_for_triple(std::string_view triple) {
  if (triple == aarch64_darwin.triple) return &aarch64_darwin;
  if (triple == x86_64_linux.triple || triple == x86_64_linux.multiarch) return &x86_64_linux;
  return nullptr;
}

}  // namespace ycxx::ast
