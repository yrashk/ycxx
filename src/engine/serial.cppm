// Serialization of keys and values for the on-disk cache (query-engine design §A7, §A10.1),
// derived by reflection with the same structural classification as fingerprints (ycxx::shape):
// integers little-endian at their own width, strings and ranges length-prefixed, aggregates
// member by member. A type whose bytes are not its identity (an interned handle) provides
// `serialize_into(Sink&, const T&)` and `deserialize_from(Source&, std::type_identity<T>)`.
//
// The reader never trusts its input (§A7.3): running past the end or reading an impossible length
// sets `failed()` and yields default values, and the caller discards the whole record.
export module ycxx.engine:serial;

import std;
import ycxx.hash;

export namespace ycxx::engine {

class Sink {
public:
  template <std::unsigned_integral U>
  void uint(U v) {
    for (std::size_t k = 0; k < sizeof(U); ++k) bytes_.push_back(static_cast<std::byte>(v >> (8 * k)));
  }
  void raw(std::span<const std::byte> b) { bytes_.append_range(b); }
  std::span<const std::byte> bytes() const noexcept { return bytes_; }

private:
  std::vector<std::byte> bytes_;
};

class Source {
public:
  explicit Source(std::span<const std::byte> b) noexcept : bytes_(b) {}

  template <std::unsigned_integral U>
  U uint() noexcept {
    if (!take(sizeof(U))) return 0;
    U v = 0;
    for (std::size_t k = 0; k < sizeof(U); ++k) v |= static_cast<U>(std::to_integer<U>(bytes_[at_ - sizeof(U) + k]) << (8 * k));
    return v;
  }
  std::span<const std::byte> raw(std::size_t n) noexcept {
    if (!take(n)) return {};
    return bytes_.subspan(at_ - n, n);
  }
  // A length read from the input, checked against what is left: each element takes at least one
  // byte, so a larger count is corrupt.
  std::size_t length() noexcept {
    const auto n = uint<std::uint64_t>();
    if (n > bytes_.size() - at_) {
      failed_ = true;
      return 0;
    }
    return static_cast<std::size_t>(n);
  }
  void fail() noexcept { failed_ = true; }
  bool failed() const noexcept { return failed_; }
  bool done() const noexcept { return at_ == bytes_.size(); }

private:
  bool take(std::size_t n) noexcept {
    if (failed_ || n > bytes_.size() - at_) {
      failed_ = true;
      return false;
    }
    at_ += n;
    return true;
  }

  std::span<const std::byte> bytes_;
  std::size_t at_ = 0;
  bool failed_ = false;
};

template <class T>
concept custom_serialized = requires(Sink& out, Source& in, const T& v) {
  serialize_into(out, v);
  { deserialize_from(in, std::type_identity<T>{}) } -> std::same_as<T>;
};

template <class T>
consteval bool serializable_type() {
  constexpr shape s = shape_of<T>();
  if constexpr (custom_serialized<T>) {
    return true;
  } else if constexpr (s == shape::none || s == shape::custom) {
    return false;  // a custom fingerprint means the bytes are not the identity: say how to store it
  } else if constexpr (s == shape::enumeration && !std::is_scoped_enum_v<T>) {
    return false;  // an out-of-range value read from corrupt input would be undefined
  } else if constexpr (s == shape::optional) {
    return serializable_type<typename T::value_type>();
  } else if constexpr (s == shape::range) {
    return serializable_type<std::remove_cvref_t<std::ranges::range_value_t<T>>>();
  } else if constexpr (s == shape::tuple) {
    return []<std::size_t... I>(std::index_sequence<I...>) {
      return (serializable_type<std::remove_cvref_t<std::tuple_element_t<I, T>>>() && ...);
    }(std::make_index_sequence<std::tuple_size_v<T>>());
  } else if constexpr (s == shape::aggregate) {
    bool ok = true;
    template for (constexpr auto m : members_of<T>()) {
      ok = ok && serializable_type<std::remove_cv_t<typename[:std::meta::type_of(m):]>>();
    }
    return ok;
  } else {
    return true;
  }
}

// Whether `serialize` and `deserialize` are derived for T.
template <class T>
concept serializable = serializable_type<std::remove_cvref_t<T>>();

template <serializable T>
void serialize(Sink& out, const T& v);
template <serializable T>
T deserialize(Source& in);

}  // namespace ycxx::engine

namespace ycxx::engine {

template <serializable T>
void serialize(Sink& out, const T& v) {
  constexpr shape s = shape_of<T>();
  if constexpr (custom_serialized<T>) {
    serialize_into(out, v);
  } else if constexpr (s == shape::fingerprint) {
    out.uint(v.lo);
    out.uint(v.hi);
  } else if constexpr (s == shape::boolean) {
    out.uint(static_cast<std::uint8_t>(v));
  } else if constexpr (s == shape::integer) {
    out.uint(static_cast<std::make_unsigned_t<T>>(v));
  } else if constexpr (s == shape::enumeration) {
    serialize(out, std::to_underlying(v));
  } else if constexpr (s == shape::floating) {
    if constexpr (sizeof(T) == 4) out.uint(std::bit_cast<std::uint32_t>(v));
    else out.uint(std::bit_cast<std::uint64_t>(v));
  } else if constexpr (s == shape::string) {
    const std::string_view str = v;
    out.uint(static_cast<std::uint64_t>(str.size()));
    out.raw(std::as_bytes(std::span(str)));
  } else if constexpr (s == shape::optional) {
    out.uint(static_cast<std::uint8_t>(v.has_value()));
    if (v) serialize(out, *v);
  } else if constexpr (s == shape::range) {
    out.uint(static_cast<std::uint64_t>(std::ranges::size(v)));
    for (const auto& e : v) serialize(out, e);
  } else if constexpr (s == shape::tuple) {
    std::apply([&](const auto&... e) { (serialize(out, e), ...); }, v);
  } else if constexpr (s == shape::aggregate) {
    template for (constexpr auto m : members_of<T>()) serialize(out, v.[:m:]);
  } else {
    static_assert(false, "ycxx::engine::serialize: no serialization for this type; provide serialize_into");
  }
}

template <serializable T>
T deserialize(Source& in) {
  constexpr shape s = shape_of<T>();
  if constexpr (custom_serialized<T>) {
    return deserialize_from(in, std::type_identity<T>{});
  } else if constexpr (s == shape::fingerprint) {
    const auto lo = in.uint<std::uint64_t>();
    return Fp128{lo, in.uint<std::uint64_t>()};
  } else if constexpr (s == shape::boolean) {
    const auto b = in.uint<std::uint8_t>();
    if (b > 1) in.fail();  // one encoding per value
    return b == 1;
  } else if constexpr (s == shape::integer) {
    return static_cast<T>(in.uint<std::make_unsigned_t<T>>());
  } else if constexpr (s == shape::enumeration) {
    return static_cast<T>(deserialize<std::underlying_type_t<T>>(in));
  } else if constexpr (s == shape::floating) {
    if constexpr (sizeof(T) == 4) return std::bit_cast<T>(in.uint<std::uint32_t>());
    else return std::bit_cast<T>(in.uint<std::uint64_t>());
  } else if constexpr (s == shape::string) {
    const auto b = in.raw(in.length());
    return T(reinterpret_cast<const char*>(b.data()), b.size());
  } else if constexpr (s == shape::optional) {
    if (in.uint<std::uint8_t>() == 0) return T{};
    return T{deserialize<typename T::value_type>(in)};
  } else if constexpr (s == shape::range) {
    using E = std::ranges::range_value_t<T>;
    const std::size_t n = in.length();
    T out{};
    if constexpr (requires { std::tuple_size<T>::value; }) {
      if (n != std::tuple_size_v<T>) {
        in.fail();
        return out;
      }
      for (auto& e : out) e = deserialize<E>(in);
    } else {
      for (std::size_t k = 0; k < n && !in.failed(); ++k) out.push_back(deserialize<E>(in));
    }
    return out;
  } else if constexpr (s == shape::tuple) {
    return [&]<std::size_t... I>(std::index_sequence<I...>) {
      return T{deserialize<std::tuple_element_t<I, T>>(in)...};
    }(std::make_index_sequence<std::tuple_size_v<T>>());
  } else if constexpr (s == shape::aggregate) {
    T out{};
    template for (constexpr auto m : members_of<T>()) {
      out.[:m:] = deserialize<std::remove_cv_t<typename[:std::meta::type_of(m):]>>(in);
    }
    return out;
  } else {
    static_assert(false, "ycxx::engine::deserialize: no serialization for this type; provide deserialize_from");
  }
}

}  // namespace ycxx::engine
