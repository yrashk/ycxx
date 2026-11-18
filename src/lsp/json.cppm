// JSON for the LSP front (#99, daemon design §6.4): a value, a parser that never trusts its
// input, a serializer, and a codec between JSON and plain aggregates derived by reflection, so
// each LSP message is a struct and nothing is written by hand per message.
//
// The codec: an aggregate is an object whose members are named after its data members; an
// `std::optional` member is omitted when empty and may be absent; integers, `bool`, strings,
// `std::vector`s and nested aggregates map the obvious way; a `Json` member passes through.
export module ycxx.lsp:json;

import std;

export namespace ycxx::lsp {

struct Json;
using Object = std::vector<std::pair<std::string, Json>>;  // in insertion order
using Array = std::vector<Json>;

struct Json {
  std::variant<std::nullptr_t, bool, double, std::string, Array, Object> v{nullptr};

  Json() = default;
  Json(std::nullptr_t) {}
  Json(bool b) : v(b) {}
  Json(double d) : v(d) {}
  template <std::integral I>
    requires (!std::same_as<I, bool>)
  Json(I i) : v(static_cast<double>(i)) {}
  Json(std::string s) : v(std::move(s)) {}
  Json(const char* s) : v(std::string(s)) {}
  Json(Array a) : v(std::move(a)) {}
  Json(Object o) : v(std::move(o)) {}

  bool is_null() const { return std::holds_alternative<std::nullptr_t>(v); }
  const std::string* string() const { return std::get_if<std::string>(&v); }
  const double* number() const { return std::get_if<double>(&v); }
  const bool* boolean() const { return std::get_if<bool>(&v); }
  const Array* array() const { return std::get_if<Array>(&v); }
  const Object* object() const { return std::get_if<Object>(&v); }
  // A member of an object, or null.
  const Json* operator[](std::string_view key) const {
    if (const Object* o = object()) {
      for (const auto& [k, val] : *o) {
        if (k == key) return &val;
      }
    }
    return nullptr;
  }
};

std::expected<Json, std::string> parse(std::string_view text);
std::string dump(const Json& j);

// The codec.
template <class T>
Json to_json(const T& v);
template <class T>
std::expected<T, std::string> from_json(const Json& j);

}  // namespace ycxx::lsp

namespace ycxx::lsp {

namespace {

class Parser {
public:
  explicit Parser(std::string_view s) : s_(s) {}

  std::expected<Json, std::string> document() {
    auto v = value(0);
    if (!v) return v;
    ws();
    if (i_ != s_.size()) return std::unexpected(err("trailing characters"));
    return v;
  }

private:
  std::string err(std::string_view what) const { return std::format("JSON: {} at offset {}", what, i_); }
  void ws() {
    while (i_ < s_.size() && (s_[i_] == ' ' || s_[i_] == '\t' || s_[i_] == '\n' || s_[i_] == '\r')) ++i_;
  }
  bool eat(std::string_view word) {
    if (s_.substr(i_, word.size()) != word) return false;
    i_ += word.size();
    return true;
  }

  std::expected<Json, std::string> value(int depth) {
    if (depth > 128) return std::unexpected(err("nested too deeply"));
    ws();
    if (i_ >= s_.size()) return std::unexpected(err("unexpected end"));
    const char c = s_[i_];
    if (c == '{') return object(depth);
    if (c == '[') return array(depth);
    if (c == '"') {
      auto s = string();
      if (!s) return std::unexpected(s.error());
      return Json(std::move(*s));
    }
    if (eat("true")) return Json(true);
    if (eat("false")) return Json(false);
    if (eat("null")) return Json(nullptr);
    return number();
  }

  std::expected<Json, std::string> number() {
    const std::size_t start = i_;
    if (i_ < s_.size() && s_[i_] == '-') ++i_;
    while (i_ < s_.size() && (std::isdigit(static_cast<unsigned char>(s_[i_])) || s_[i_] == '.' || s_[i_] == 'e' ||
                              s_[i_] == 'E' || s_[i_] == '+' || s_[i_] == '-')) {
      ++i_;
    }
    double d = 0;
    const auto r = std::from_chars(s_.data() + start, s_.data() + i_, d);
    if (i_ == start || r.ec != std::errc{} || r.ptr != s_.data() + i_) return std::unexpected(err("bad number"));
    return Json(d);
  }

  static void utf8(std::string& out, std::uint32_t cp) {
    if (cp < 0x80) {
      out += static_cast<char>(cp);
    } else if (cp < 0x800) {
      out += static_cast<char>(0xc0 | (cp >> 6));
      out += static_cast<char>(0x80 | (cp & 0x3f));
    } else if (cp < 0x10000) {
      out += static_cast<char>(0xe0 | (cp >> 12));
      out += static_cast<char>(0x80 | ((cp >> 6) & 0x3f));
      out += static_cast<char>(0x80 | (cp & 0x3f));
    } else {
      out += static_cast<char>(0xf0 | (cp >> 18));
      out += static_cast<char>(0x80 | ((cp >> 12) & 0x3f));
      out += static_cast<char>(0x80 | ((cp >> 6) & 0x3f));
      out += static_cast<char>(0x80 | (cp & 0x3f));
    }
  }

  std::optional<std::uint32_t> hex4() {
    if (i_ + 4 > s_.size()) return std::nullopt;
    std::uint32_t v = 0;
    const auto r = std::from_chars(s_.data() + i_, s_.data() + i_ + 4, v, 16);
    if (r.ec != std::errc{} || r.ptr != s_.data() + i_ + 4) return std::nullopt;
    i_ += 4;
    return v;
  }

  std::expected<std::string, std::string> string() {
    ++i_;  // "
    std::string out;
    while (i_ < s_.size()) {
      const char c = s_[i_++];
      if (c == '"') return out;
      if (static_cast<unsigned char>(c) < 0x20) return std::unexpected(err("control character in a string"));
      if (c != '\\') {
        out += c;
        continue;
      }
      if (i_ >= s_.size()) break;
      const char e = s_[i_++];
      switch (e) {
        case '"': out += '"'; break;
        case '\\': out += '\\'; break;
        case '/': out += '/'; break;
        case 'b': out += '\b'; break;
        case 'f': out += '\f'; break;
        case 'n': out += '\n'; break;
        case 'r': out += '\r'; break;
        case 't': out += '\t'; break;
        case 'u': {
          auto cp = hex4();
          if (!cp) return std::unexpected(err("bad \\u escape"));
          if (*cp >= 0xd800 && *cp < 0xdc00) {  // a surrogate pair
            if (!eat("\\u")) return std::unexpected(err("lone surrogate"));
            auto lo = hex4();
            if (!lo || *lo < 0xdc00 || *lo >= 0xe000) return std::unexpected(err("bad surrogate pair"));
            *cp = 0x10000 + ((*cp - 0xd800) << 10) + (*lo - 0xdc00);
          }
          utf8(out, *cp);
          break;
        }
        default: return std::unexpected(err("bad escape"));
      }
    }
    return std::unexpected(err("unterminated string"));
  }

  std::expected<Json, std::string> array(int depth) {
    ++i_;  // [
    Array out;
    ws();
    if (i_ < s_.size() && s_[i_] == ']') {
      ++i_;
      return Json(std::move(out));
    }
    while (true) {
      auto v = value(depth + 1);
      if (!v) return v;
      out.push_back(std::move(*v));
      ws();
      if (i_ < s_.size() && s_[i_] == ',') {
        ++i_;
        continue;
      }
      if (i_ < s_.size() && s_[i_] == ']') {
        ++i_;
        return Json(std::move(out));
      }
      return std::unexpected(err("expected , or ]"));
    }
  }

  std::expected<Json, std::string> object(int depth) {
    ++i_;  // {
    Object out;
    ws();
    if (i_ < s_.size() && s_[i_] == '}') {
      ++i_;
      return Json(std::move(out));
    }
    while (true) {
      ws();
      if (i_ >= s_.size() || s_[i_] != '"') return std::unexpected(err("expected a member name"));
      auto k = string();
      if (!k) return std::unexpected(k.error());
      ws();
      if (i_ >= s_.size() || s_[i_] != ':') return std::unexpected(err("expected :"));
      ++i_;
      auto v = value(depth + 1);
      if (!v) return v;
      out.emplace_back(std::move(*k), std::move(*v));
      ws();
      if (i_ < s_.size() && s_[i_] == ',') {
        ++i_;
        continue;
      }
      if (i_ < s_.size() && s_[i_] == '}') {
        ++i_;
        return Json(std::move(out));
      }
      return std::unexpected(err("expected , or }"));
    }
  }

  std::string_view s_;
  std::size_t i_ = 0;
};

void dump_string(std::string& out, std::string_view s) {
  out += '"';
  for (const char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) out += std::format("\\u{:04x}", static_cast<unsigned>(static_cast<unsigned char>(c)));
        else out += c;
    }
  }
  out += '"';
}

void dump_into(std::string& out, const Json& j) {
  std::visit(
      [&](const auto& x) {
        using X = std::remove_cvref_t<decltype(x)>;
        if constexpr (std::is_same_v<X, std::nullptr_t>) {
          out += "null";
        } else if constexpr (std::is_same_v<X, bool>) {
          out += x ? "true" : "false";
        } else if constexpr (std::is_same_v<X, double>) {
          if (x == std::floor(x) && std::abs(x) < 1e15) out += std::format("{}", static_cast<std::int64_t>(x));
          else out += std::format("{}", x);
        } else if constexpr (std::is_same_v<X, std::string>) {
          dump_string(out, x);
        } else if constexpr (std::is_same_v<X, Array>) {
          out += '[';
          for (std::size_t i = 0; i < x.size(); ++i) {
            if (i) out += ',';
            dump_into(out, x[i]);
          }
          out += ']';
        } else {
          out += '{';
          for (std::size_t i = 0; i < x.size(); ++i) {
            if (i) out += ',';
            dump_string(out, x[i].first);
            out += ':';
            dump_into(out, x[i].second);
          }
          out += '}';
        }
      },
      j.v);
}

}  // namespace

// For the codec's templates, which importers instantiate: not TU-local.
namespace codec {
template <class T>
constexpr bool is_optional = false;
template <class T>
constexpr bool is_optional<std::optional<T>> = true;
template <class T>
constexpr bool is_vector = false;
template <class T>
constexpr bool is_vector<std::vector<T>> = true;

template <class T>
consteval auto fields() {
  return std::define_static_array(std::meta::nonstatic_data_members_of(^^T, std::meta::access_context::unchecked()));
}
}  // namespace codec

std::expected<Json, std::string> parse(std::string_view text) { return Parser(text).document(); }

std::string dump(const Json& j) {
  std::string out;
  dump_into(out, j);
  return out;
}

template <class T>
Json to_json(const T& v) {
  if constexpr (std::is_same_v<T, Json>) {
    return v;
  } else if constexpr (std::is_same_v<T, bool> || std::is_same_v<T, std::string>) {
    return Json(v);
  } else if constexpr (std::is_integral_v<T> || std::is_floating_point_v<T>) {
    return Json(static_cast<double>(v));
  } else if constexpr (codec::is_optional<T>) {
    return v ? to_json(*v) : Json(nullptr);
  } else if constexpr (codec::is_vector<T>) {
    Array a;
    for (const auto& x : v) a.push_back(to_json(x));
    return Json(std::move(a));
  } else {
    static_assert(std::is_aggregate_v<T>, "an LSP message is an aggregate");
    Object o;
    template for (constexpr auto m : codec::fields<T>()) {
      const auto& member = v.[:m:];
      if constexpr (codec::is_optional<std::remove_cvref_t<decltype(member)>>) {
        if (!member) continue;
      }
      o.emplace_back(std::string(std::meta::identifier_of(m)), to_json(member));
    }
    return Json(std::move(o));
  }
}

template <class T>
std::expected<T, std::string> from_json(const Json& j) {
  if constexpr (std::is_same_v<T, Json>) {
    return j;
  } else if constexpr (std::is_same_v<T, bool>) {
    if (const bool* b = j.boolean()) return *b;
    return std::unexpected("expected a boolean");
  } else if constexpr (std::is_same_v<T, std::string>) {
    if (const std::string* s = j.string()) return *s;
    return std::unexpected("expected a string");
  } else if constexpr (std::is_integral_v<T>) {
    const double* d = j.number();
    // The bounds are exact doubles: `max()` would round up to 2^digits, which does not fit.
    if (!d || *d != std::floor(*d) || *d < static_cast<double>(std::numeric_limits<T>::min()) ||
        *d >= std::ldexp(1.0, std::numeric_limits<T>::digits)) {
      return std::unexpected("expected an integer in range");
    }
    return static_cast<T>(*d);
  } else if constexpr (codec::is_optional<T>) {
    if (j.is_null()) return T{};
    auto inner = from_json<typename T::value_type>(j);
    if (!inner) return std::unexpected(inner.error());
    return T(std::move(*inner));
  } else if constexpr (codec::is_vector<T>) {
    const Array* a = j.array();
    if (!a) return std::unexpected("expected an array");
    T out;
    for (const Json& x : *a) {
      auto e = from_json<typename T::value_type>(x);
      if (!e) return std::unexpected(e.error());
      out.push_back(std::move(*e));
    }
    return out;
  } else {
    static_assert(std::is_aggregate_v<T>, "an LSP message is an aggregate");
    if (!j.object()) return std::unexpected("expected an object");
    T out{};
    std::string error;
    template for (constexpr auto m : codec::fields<T>()) {
      using M = std::remove_cvref_t<decltype(out.[:m:])>;
      if (error.empty()) {
        const Json* field = j[std::meta::identifier_of(m)];
        if (!field) {
          if constexpr (!codec::is_optional<M>) error = std::format("missing member {}", std::meta::identifier_of(m));
        } else if (auto e = from_json<M>(*field)) {
          out.[:m:] = std::move(*e);
        } else {
          error = std::format("{}: {}", std::meta::identifier_of(m), e.error());
        }
      }
    }
    if (!error.empty()) return std::unexpected(error);
    return out;
  }
}

}  // namespace ycxx::lsp
