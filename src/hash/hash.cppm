// ycxx's one hash: 128-bit fingerprints with a fixed seed (query-engine design §A1.2, §A1.3, §A5.1).
//
// The function is MurmurHash3_x64_128 (Austin Appleby, SMHasher, public domain), implemented here
// from its published description; the choice and its measurements are recorded on #14. Every
// fingerprint ycxx computes (memo keys and values, interned nodes, the on-disk cache) goes through
// `Hasher`, so the seed is fixed and nothing depends on `std::hash`, addresses or the process.
//
// `fingerprint(v)` derives a structural fingerprint for any value by reflection: integers and
// enumerations by value, ranges and strings by length and elements, aggregates member by member.
// A type whose bytes are not its identity (an interned handle, whose number depends on allocation
// order) provides `hash_into(Hasher&, const T&)`, found by argument-dependent lookup, and appends
// its Merkle fingerprint instead.
export module ycxx.hash;

import std;

export namespace ycxx {

// A 128-bit fingerprint. `lo` is the first 64-bit half of MurmurHash3's output, `hi` the second.
struct Fp128 {
  std::uint64_t lo = 0;
  std::uint64_t hi = 0;
  // BOOTSTRAP: modules: a defaulted *friend* comparison in an exported class ICEs GCC 16.2 in
  // the importer (STATUS, "Bootstrap workarounds"); member operators work.
  constexpr bool operator==(const Fp128&) const = default;
  constexpr auto operator<=>(const Fp128&) const = default;
};

// The seed of every ycxx fingerprint ("ycxx" in ASCII). Changing it changes the on-disk cache's
// keys, so it is part of the cache schema (§A7.1).
inline constexpr std::uint32_t hash_seed = 0x79637878;

// A streaming MurmurHash3_x64_128. Appending bytes in pieces gives the same result as appending
// them at once, so structured values can be hashed without first being flattened.
class Hasher {
public:
  constexpr explicit Hasher(std::uint32_t seed = hash_seed) noexcept : h1_(seed), h2_(seed) {}

  // A local hasher serves in-memory hash tables of one process (`local_hash`): a handle whose
  // fingerprint lives in an interner is hashed by its number, which is cheaper and needs no
  // interner, and is never stored or compared across processes.
  static constexpr Hasher local_hasher() noexcept {
    Hasher h;
    h.local_ = true;
    return h;
  }
  constexpr bool local() const noexcept { return local_; }

  template <class Char>
    requires (sizeof(Char) == 1 && (std::integral<Char> || std::same_as<Char, std::byte>))
  constexpr Hasher& bytes(std::span<const Char> in) noexcept {
    std::size_t i = 0;
    const std::size_t n = in.size();
    if (fill_ != 0) {
      while (fill_ < 16 && i < n) buf_[fill_++] = static_cast<std::uint8_t>(in[i++]);
      if (fill_ < 16) {
        len_ += n;
        return *this;
      }
      block(load64(buf_.data()), load64(buf_.data() + 8));
      fill_ = 0;
    }
    for (; i + 16 <= n; i += 16) block(load64(in.data() + i), load64(in.data() + i + 8));
    while (i < n) buf_[fill_++] = static_cast<std::uint8_t>(in[i++]);
    len_ += n;
    return *this;
  }
  constexpr Hasher& bytes(std::string_view s) noexcept { return bytes(std::span<const char>(s)); }

  // Fixed-width little-endian integers, so a fingerprint is the same on every host.
  template <std::unsigned_integral U>
  constexpr Hasher& uint(U v) noexcept {
    std::array<std::uint8_t, sizeof(U)> b{};
    for (std::size_t k = 0; k < sizeof(U); ++k) b[k] = static_cast<std::uint8_t>(v >> (8 * k));
    return bytes(std::span<const std::uint8_t>(b));
  }
  constexpr Hasher& fp(Fp128 f) noexcept { return uint(f.lo).uint(f.hi); }

  constexpr Fp128 finish() const noexcept {
    std::uint64_t h1 = h1_, h2 = h2_, k1 = 0, k2 = 0;
    for (std::size_t k = fill_; k-- > 8;) k2 = (k2 << 8) | buf_[k];
    for (std::size_t k = std::min<std::size_t>(fill_, 8); k-- > 0;) k1 = (k1 << 8) | buf_[k];
    if (fill_ > 8) h2 ^= std::rotl(k2 * c2, 33) * c1;
    if (fill_ > 0) h1 ^= std::rotl(k1 * c1, 31) * c2;
    h1 ^= len_;
    h2 ^= len_;
    h1 += h2;
    h2 += h1;
    h1 = fmix(h1);
    h2 = fmix(h2);
    h1 += h2;
    h2 += h1;
    return {h1, h2};
  }

private:
  static constexpr std::uint64_t c1 = 0x87c37b91114253d5;
  static constexpr std::uint64_t c2 = 0x4cf5ad432745937f;

  template <class Char>
  static constexpr std::uint64_t load64(const Char* p) noexcept {
    if consteval {
      std::uint64_t v = 0;
      for (int k = 7; k >= 0; --k) v = (v << 8) | static_cast<std::uint8_t>(p[k]);
      return v;
    } else {
      std::uint64_t v;
      std::memcpy(&v, p, 8);
      if constexpr (std::endian::native == std::endian::big) v = std::byteswap(v);
      return v;
    }
  }
  static constexpr std::uint64_t fmix(std::uint64_t k) noexcept {
    k ^= k >> 33;
    k *= 0xff51afd7ed558ccd;
    k ^= k >> 33;
    k *= 0xc4ceb9fe1a85ec53;
    k ^= k >> 33;
    return k;
  }
  constexpr void block(std::uint64_t k1, std::uint64_t k2) noexcept {
    h1_ ^= std::rotl(k1 * c1, 31) * c2;
    h1_ = std::rotl(h1_, 27) + h2_;
    h1_ = h1_ * 5 + 0x52dce729;
    h2_ ^= std::rotl(k2 * c2, 33) * c1;
    h2_ = std::rotl(h2_, 31) + h1_;
    h2_ = h2_ * 5 + 0x38495ab5;
  }

  std::uint64_t h1_;
  std::uint64_t h2_;
  std::uint64_t len_ = 0;
  std::array<std::uint8_t, 16> buf_{};
  std::size_t fill_ = 0;
  bool local_ = false;
};

// The fingerprint of a byte string.
template <class Char>
  requires (sizeof(Char) == 1 && (std::integral<Char> || std::same_as<Char, std::byte>))
constexpr Fp128 fingerprint128(std::span<const Char> in, std::uint32_t seed = hash_seed) noexcept {
  return Hasher(seed).bytes(in).finish();
}
constexpr Fp128 fingerprint128(std::string_view s, std::uint32_t seed = hash_seed) noexcept {
  return Hasher(seed).bytes(s).finish();
}

// A type with its own fingerprint (an interned handle) provides `hash_into(Hasher&, const T&)`.
template <class T>
concept custom_hashed = requires(Hasher& h, const T& v) { hash_into(h, v); };

// How a value is taken apart structurally, for fingerprints here and for serialization in the
// engine: one classification, so the two cannot disagree on what a type is made of.
enum class shape { custom, fingerprint, boolean, integer, enumeration, floating, string, optional, range, tuple, aggregate, none };

template <class T>
inline constexpr bool is_optional = false;
template <class T>
inline constexpr bool is_optional<std::optional<T>> = true;

// Unordered containers iterate in an order that depends on the process (§A5.1).
template <class T>
concept unordered_container = requires { typename T::hasher; typename T::key_equal; };

// An aggregate with base classes is not taken apart: `nonstatic_data_members_of` does not see
// the bases' members, so they would be silently left out.
template <class T>
consteval bool has_bases() {
  return !std::meta::bases_of(^^T, std::meta::access_context::unchecked()).empty();
}

template <class T>
consteval shape shape_of() {
  if constexpr (custom_hashed<T>) return shape::custom;
  else if constexpr (std::is_pointer_v<T> || unordered_container<T>) return shape::none;
  else if constexpr (std::same_as<T, Fp128>) return shape::fingerprint;
  else if constexpr (std::same_as<T, bool>) return shape::boolean;
  else if constexpr (std::integral<T>) return shape::integer;
  else if constexpr (std::is_enum_v<T>) return shape::enumeration;
  else if constexpr (std::floating_point<T> && (sizeof(T) == 4 || sizeof(T) == 8)) return shape::floating;
  else if constexpr (std::convertible_to<const T&, std::string_view>) return shape::string;
  else if constexpr (is_optional<T>) return shape::optional;
  else if constexpr (std::ranges::sized_range<const T&>) return shape::range;
  else if constexpr (requires { std::tuple_size<T>::value; }) return shape::tuple;
  else if constexpr (std::is_aggregate_v<T> && std::is_class_v<T> && !has_bases<T>()) return shape::aggregate;
  else return shape::none;
}

// The non-static data members of an aggregate, as a static array `template for` can expand.
template <class T>
consteval auto members_of() {
  return std::define_static_array(std::meta::nonstatic_data_members_of(^^T, std::meta::access_context::unchecked()));
}

template <class T>
consteval bool fingerprintable_type() {
  constexpr shape s = shape_of<T>();
  if constexpr (s == shape::none) {
    return false;
  } else if constexpr (s == shape::optional || s == shape::range) {
    return fingerprintable_type<std::remove_cvref_t<std::ranges::range_value_t<
        std::conditional_t<s == shape::optional, std::array<typename T::value_type, 1>, T>>>>();
  } else if constexpr (s == shape::tuple) {
    return []<std::size_t... I>(std::index_sequence<I...>) {
      return (fingerprintable_type<std::remove_cvref_t<std::tuple_element_t<I, T>>>() && ...);
    }(std::make_index_sequence<std::tuple_size_v<T>>());
  } else if constexpr (s == shape::aggregate) {
    bool ok = true;
    template for (constexpr auto m : members_of<T>()) {
      ok = ok && fingerprintable_type<std::remove_cv_t<typename[:std::meta::type_of(m):]>>();
    }
    return ok;
  } else {
    return true;
  }
}

// Whether `fingerprint` and `hash_append` derive a fingerprint for T.
template <class T>
concept fingerprintable = fingerprintable_type<std::remove_cvref_t<T>>();

// Appends the structural fingerprint of `v`, derived by reflection where the type has no
// `hash_into` of its own. Sizes are appended before elements, so concatenations do not collide.
template <fingerprintable T>
constexpr void hash_append(Hasher& h, const T& v) noexcept {
  constexpr shape s = shape_of<T>();
  if constexpr (s == shape::custom) {
    // A handle (an enumeration, or a 32-bit id) in a local hash: its number.
    if constexpr (std::is_enum_v<T>) {
      if (h.local()) return void(h.uint(static_cast<std::make_unsigned_t<std::underlying_type_t<T>>>(std::to_underlying(v))));
    } else if constexpr (std::is_trivially_copyable_v<T> && sizeof(T) == 4) {
      if (h.local()) return void(h.uint(std::bit_cast<std::uint32_t>(v)));
    }
    hash_into(h, v);
  } else if constexpr (s == shape::fingerprint) {
    h.fp(v);
  } else if constexpr (s == shape::boolean) {
    h.uint(static_cast<std::uint8_t>(v));
  } else if constexpr (s == shape::integer) {
    h.uint(static_cast<std::make_unsigned_t<T>>(v));
  } else if constexpr (s == shape::enumeration) {
    hash_append(h, std::to_underlying(v));
  } else if constexpr (s == shape::floating) {
    if constexpr (sizeof(T) == 4) h.uint(std::bit_cast<std::uint32_t>(v));
    else h.uint(std::bit_cast<std::uint64_t>(v));
  } else if constexpr (s == shape::string) {
    const std::string_view str = v;
    h.uint(static_cast<std::uint64_t>(str.size()));
    h.bytes(str);
  } else if constexpr (s == shape::optional) {
    h.uint(static_cast<std::uint8_t>(v.has_value()));
    if (v) hash_append(h, *v);
  } else if constexpr (s == shape::range) {
    h.uint(static_cast<std::uint64_t>(std::ranges::size(v)));
    for (const auto& e : v) hash_append(h, e);
  } else if constexpr (s == shape::tuple) {
    std::apply([&](const auto&... e) { (hash_append(h, e), ...); }, v);
  } else {
    template for (constexpr auto m : members_of<T>()) hash_append(h, v.[:m:]);
  }
}

// The structural fingerprint of a value.
template <fingerprintable T>
constexpr Fp128 fingerprint(const T& v) noexcept {
  Hasher h;
  hash_append(h, v);
  return h.finish();
}

// A hash for in-memory tables of this process: like `fingerprint`, but handles count by their
// numbers (`Hasher::local_hasher`). Never stored, never compared across processes or sessions.
template <fingerprintable T>
constexpr Fp128 local_hash(const T& v) noexcept {
  Hasher h = Hasher::local_hasher();
  hash_append(h, v);
  return h.finish();
}

}  // namespace ycxx
