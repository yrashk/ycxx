// The controlling expression of `#if` and `#elif` ([cpp.cond]/12–15), after macro replacement and
// after `defined` and the `__has_*` operators have become numbers (:unit): an integral constant
// expression evaluated in `std::intmax_t` and `std::uintmax_t`, where an identifier left over is 0
// (but `true` is 1).
export module ycxx.pp:cond;

import std;
import ycxx.lex;
import :spell;
import :region;

export namespace ycxx::pp {

class Condition {
public:
  using Report = std::function<void(const PTok&, lex::LexDiag)>;

  Condition(std::span<const PTok> toks, const Spellings& s, bool char_signed, Report report)
      : toks_(toks), s_(&s), char_signed_(char_signed), report_(std::move(report)) {}

  // The value, or nothing after reporting why there is none.
  std::optional<bool> run() {
    auto v = conditional(true);
    if (!v) return std::nullopt;
    if (pos_ < toks_.size()) return fail(toks_[pos_], lex::LexDiag::cond_bad_token);
    return v->bits != 0;
  }

private:
  // A value of type `intmax_t` or `uintmax_t` ([cpp.cond]/14), as its bits.
  struct Value {
    std::uint64_t bits = 0;
    bool uns = false;
  };

  std::nullopt_t fail(const PTok& at, lex::LexDiag d) {
    if (!failed_) report_(at, d);
    failed_ = true;
    return std::nullopt;
  }
  const PTok* peek() const { return pos_ < toks_.size() ? &toks_[pos_] : nullptr; }
  bool at(Tk k) const { return pos_ < toks_.size() && toks_[pos_].tok.kind == k; }
  const PTok& last() const { return toks_.empty() ? dummy_ : toks_[std::min(pos_, toks_.size() - 1)]; }

  // `live`: evaluated; an unevaluated operand's division by zero is not an error.
  std::optional<Value> conditional(bool live) {
    auto c = binary(1, live);
    if (!c || !at(Tk::question)) return c;
    ++pos_;
    bool t = c->bits != 0;
    auto a = conditional(live && t);
    if (!a) return std::nullopt;
    if (!at(Tk::colon)) return fail(last(), lex::LexDiag::cond_expected_colon);
    ++pos_;
    auto b = conditional(live && !t);
    if (!b) return std::nullopt;
    Value r = t ? *a : *b;
    r.uns = a->uns || b->uns;
    return r;
  }

  static int precedence(Tk k) {
    switch (k) {
    case Tk::pipepipe: return 1;
    case Tk::ampamp: return 2;
    case Tk::pipe: return 3;
    case Tk::caret: return 4;
    case Tk::amp: return 5;
    case Tk::equalequal:
    case Tk::exclaimequal: return 6;
    case Tk::less:
    case Tk::greater:
    case Tk::lessequal:
    case Tk::greaterequal: return 7;
    case Tk::lessless:
    case Tk::greatergreater: return 8;
    case Tk::plus:
    case Tk::minus: return 9;
    case Tk::star:
    case Tk::slash:
    case Tk::percent: return 10;
    default: return 0;
    }
  }

  std::optional<Value> binary(int min, bool live) {
    auto lhs = unary(live);
    for (;;) {
      if (!lhs) return std::nullopt;
      const PTok* op = peek();
      int p = op ? precedence(op->tok.kind) : 0;
      if (p < min || p == 0) return lhs;
      ++pos_;
      Tk k = op->tok.kind;
      // `&&` and `||` evaluate their right operand only when it decides.
      bool rhs_live = live && !(k == Tk::ampamp && lhs->bits == 0) && !(k == Tk::pipepipe && lhs->bits != 0);
      auto rhs = binary(p + 1, rhs_live);
      if (!rhs) return std::nullopt;
      lhs = apply(*op, *lhs, *rhs, live);
    }
  }

  std::optional<Value> apply(const PTok& op, Value a, Value b, bool live) {
    const bool uns = a.uns || b.uns;  // the usual arithmetic conversions
    const auto sa = static_cast<std::int64_t>(a.bits), sb = static_cast<std::int64_t>(b.bits);
    auto boolean = [](bool v) { return Value{v ? 1u : 0u, false}; };
    switch (op.tok.kind) {
    case Tk::pipepipe: return boolean(a.bits != 0 || b.bits != 0);
    case Tk::ampamp: return boolean(a.bits != 0 && b.bits != 0);
    case Tk::pipe: return Value{a.bits | b.bits, uns};
    case Tk::caret: return Value{a.bits ^ b.bits, uns};
    case Tk::amp: return Value{a.bits & b.bits, uns};
    case Tk::equalequal: return boolean(a.bits == b.bits);
    case Tk::exclaimequal: return boolean(a.bits != b.bits);
    case Tk::less: return boolean(uns ? a.bits < b.bits : sa < sb);
    case Tk::greater: return boolean(uns ? a.bits > b.bits : sa > sb);
    case Tk::lessequal: return boolean(uns ? a.bits <= b.bits : sa <= sb);
    case Tk::greaterequal: return boolean(uns ? a.bits >= b.bits : sa >= sb);
    case Tk::lessless: return Value{b.bits >= 64 ? 0 : a.bits << b.bits, a.uns};
    case Tk::greatergreater:
      if (a.uns) return Value{b.bits >= 64 ? 0 : a.bits >> b.bits, true};
      return Value{static_cast<std::uint64_t>(sa >> std::min<std::uint64_t>(b.bits, 63)), false};
    case Tk::plus: return Value{a.bits + b.bits, uns};
    case Tk::minus: return Value{a.bits - b.bits, uns};
    case Tk::star: return Value{a.bits * b.bits, uns};
    case Tk::slash:
    case Tk::percent: {
      if (b.bits == 0) {
        if (live) return fail(op, lex::LexDiag::cond_division_by_zero);
        return Value{0, uns};
      }
      bool div = op.tok.kind == Tk::slash;
      if (uns) return Value{div ? a.bits / b.bits : a.bits % b.bits, true};
      if (sa == std::numeric_limits<std::int64_t>::min() && sb == -1) return Value{div ? a.bits : 0, false};
      return Value{static_cast<std::uint64_t>(div ? sa / sb : sa % sb), false};
    }
    default: std::unreachable();
    }
  }

  std::optional<Value> unary(bool live) {
    const PTok* t = peek();
    if (!t) return fail(last(), lex::LexDiag::cond_expected_value);
    switch (t->tok.kind) {
    case Tk::plus:
    case Tk::minus:
    case Tk::tilde:
    case Tk::exclaim: {
      ++pos_;
      auto v = unary(live);
      if (!v) return std::nullopt;
      if (t->tok.kind == Tk::minus) v->bits = 0 - v->bits;
      else if (t->tok.kind == Tk::tilde) v->bits = ~v->bits;
      else if (t->tok.kind == Tk::exclaim) *v = Value{v->bits == 0 ? 1u : 0u, false};
      return v;
    }
    case Tk::l_paren: {
      ++pos_;
      auto v = conditional(live);
      // `( expression )`: a comma expression has the value of its last operand.
      while (v && at(Tk::comma)) {
        ++pos_;
        v = conditional(live);
      }
      if (!v) return std::nullopt;
      if (!at(Tk::r_paren)) return fail(last(), lex::LexDiag::cond_expected_rparen);
      ++pos_;
      return v;
    }
    case Tk::identifier: {
      ++pos_;
      return Value{s_->name(t->tok.payload) == "true" ? 1u : 0u, false};
    }
    case Tk::pp_number: ++pos_; return number(*t);
    case Tk::char_lit: ++pos_; return character(*t);
    default: return fail(*t, lex::LexDiag::cond_bad_token);
    }
  }

  // [cpp.cond]/14: an integer literal; a floating one, or one with a ud-suffix, is no value here.
  std::optional<Value> number(const PTok& t) {
    Literal l = s_->literal(t.tok.payload);
    lex::Tables tables;
    if (l.ud_suffix != lex::no_name) return fail(t, lex::LexDiag::cond_bad_token);
    bool bad = false;
    Tk k = lex::classify_number(l, tables, [&](u32, lex::LexDiag d, u64, u64) {
      if (!bad) report_(t, d);
      bad = true;
    });
    if (bad) {
      failed_ = true;
      return std::nullopt;
    }
    if (k != Tk::int_lit) return fail(t, lex::LexDiag::cond_bad_token);
    bool uns = l.int_suffix == lex::IntSuffix::u || l.int_suffix == lex::IntSuffix::ul ||
               l.int_suffix == lex::IntSuffix::ull || l.int_suffix == lex::IntSuffix::uz ||
               l.value > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
    return Value{l.value, uns};
  }

  std::optional<Value> character(const PTok& t) {
    Literal l = s_->literal(t.tok.payload);
    if (l.ud_suffix != lex::no_name) return fail(t, lex::LexDiag::cond_bad_token);
    bool bad = false;
    lex::evaluate_char(l, [&](u32, lex::LexDiag d, u64, u64) {
      if (lex::message_of(d).severity == lex::Severity::error) {
        if (!bad) report_(t, d);
        bad = true;
      }
    });
    if (bad) {
      failed_ = true;
      return std::nullopt;
    }
    // Converted to the literal's type, then to intmax_t: an ordinary `char` is signed on targets
    // where it is; a multicharacter literal is an `int`.
    std::uint64_t v = l.value;
    if (l.multichar || l.encoding == lex::Encoding::wide) v = static_cast<std::uint64_t>(static_cast<std::int32_t>(v));
    else if (l.encoding == lex::Encoding::ordinary && char_signed_) v = static_cast<std::uint64_t>(static_cast<std::int8_t>(v));
    return Value{v, false};
  }

  std::span<const PTok> toks_;
  const Spellings* s_;
  bool char_signed_;
  Report report_;
  std::size_t pos_ = 0;
  bool failed_ = false;
  PTok dummy_{};
};

}  // namespace ycxx::pp
