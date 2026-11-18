// The pattern table (optimization design §5): `aarch64.patterns`, embedded with `#embed` and read
// by a `constexpr` parser. A `static_assert` parses it at compile time, so a malformed table is a
// build error; `patterns()` parses it once at run time for the selector. The rule syntax is
// described at the top of the table.
export module ycxx.target.aarch64:patterns;

import std;
import ycxx.ir;
import ycxx.ir.l2;
import :isa;

namespace ycxx::aarch64 {

// A left-side node: an L2 op with its immediate (unbound, bound to a variable, or a literal) and
// its value operands; or a variable matching any value.
export struct PNode {
  bool is_var = false;
  std::uint8_t var = 0;  // for a variable; for an op, the variable its immediate binds
  l2::Op op{};
  enum class ImmKind : std::uint8_t { none, var, lit } imm = ImmKind::none;
  std::int64_t lit = 0;
  std::uint16_t first = 0, count = 0;  // children, in `Table::children`
};

export enum class RKind : std::uint8_t { var, dst, temp, lit, neg, msb, zr, sp, cond, succ };
export struct ROp {
  RKind kind = RKind::lit;
  std::int64_t v = 0;  // variable, temporary or successor number; literal; condition
};
export struct RInst {
  Form form{};
  ir::List<ROp, 4> ops;
};

export enum class CKind : std::uint8_t { uimm12, nuimm12, uoff, nonneg, local, extern_, wide, narrow, signed_, unsigned_ };
export struct PCond {
  CKind kind{};
  std::uint8_t var = 0;
};

export struct Pattern {
  std::string_view name;
  std::uint16_t root = 0;  // in `Table::nodes`
  std::uint16_t first_inst = 0, ninsts = 0;
  std::uint16_t first_cond = 0, nconds = 0;
  std::uint16_t cost = 0;
  std::uint16_t size = 0;  // L2 nodes the left side covers
  std::uint8_t nvars = 0;
  std::vector<std::string_view> var_names;
};

export struct Table {
  std::vector<Pattern> patterns;
  std::vector<PNode> nodes;
  std::vector<std::uint16_t> children;
  std::vector<RInst> insts;
  std::vector<PCond> conds;
};

export struct TableError {
  std::uint32_t line = 0;
  std::string_view message;
};

constexpr bool space(char c) { return c == ' ' || c == '\t' || c == '\r'; }
constexpr bool word(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
}

constexpr std::optional<std::int64_t> number(std::string_view s) {
  bool neg = false;
  if (!s.empty() && s[0] == '-') {
    neg = true;
    s.remove_prefix(1);
  }
  if (s.empty()) return std::nullopt;
  std::int64_t v = 0;
  for (char c : s) {
    if (c < '0' || c > '9') return std::nullopt;
    v = v * 10 + (c - '0');
  }
  return neg ? -v : v;
}

constexpr std::optional<l2::Op> l2_op(std::string_view name) {
  for (std::size_t i = 0; i < l2::op_count; ++i)
    if (l2::op_table[i].name == name) return l2::Op(i);
  return std::nullopt;
}

constexpr std::optional<Cond> cond_named(std::string_view s) {
  for (std::size_t i = 0; i < std::size(cond_names); ++i)
    if (cond_names[i] == s) return Cond(i);
  return std::nullopt;
}

// One line's worth of parsing state.
struct LineParser {
  std::string_view s;
  std::size_t at = 0;
  Table& t;
  Pattern& p;
  std::string_view error;

  constexpr void skip() {
    while (at < s.size() && space(s[at])) ++at;
  }
  constexpr bool eat(std::string_view tok) {
    skip();
    if (s.substr(at, tok.size()) != tok) return false;
    at += tok.size();
    return true;
  }
  constexpr std::string_view ident() {
    skip();
    std::size_t b = at;
    while (at < s.size() && word(s[at])) ++at;
    return s.substr(b, at - b);
  }
  constexpr bool fail(std::string_view why) {
    if (error.empty()) error = why;
    return false;
  }
  constexpr std::optional<std::uint8_t> var(bool define) {
    std::string_view n = ident();
    if (n.empty()) return std::nullopt;
    for (std::size_t i = 0; i < p.var_names.size(); ++i)
      if (p.var_names[i] == n) return std::uint8_t(i);
    if (!define) return std::nullopt;
    p.var_names.push_back(n);
    return std::uint8_t(p.var_names.size() - 1);
  }

  // A left-side tree; returns its node index.
  constexpr std::optional<std::uint16_t> tree() {
    if (eat("?")) {
      auto v = var(true);
      if (!v) return fail("expected a variable name"), std::nullopt;
      t.nodes.push_back({true, *v});
      return std::uint16_t(t.nodes.size() - 1);
    }
    if (!eat("(")) return fail("expected `(` or `?`"), std::nullopt;
    auto op = l2_op(ident());
    if (!op) return fail("unknown L2 op"), std::nullopt;
    PNode n;
    n.op = *op;
    std::size_t nimm = 0;
    for (l2::Imm k : l2::spec_of(*op).imm.view())
      if (k != l2::Imm::alias) nimm = 1;
    if (nimm) {
      skip();
      if (eat("?")) {
        auto v = var(true);
        if (!v) return fail("expected a variable name"), std::nullopt;
        n.imm = PNode::ImmKind::var;
        n.var = *v;
      } else {
        auto lit = number(ident());
        if (!lit) return fail("expected an immediate variable or literal"), std::nullopt;
        n.imm = PNode::ImmKind::lit;
        n.lit = *lit;
      }
    }
    std::vector<std::uint16_t> kids;
    while (!eat(")")) {
      if (at >= s.size()) return fail("unclosed `(`"), std::nullopt;
      auto k = tree();
      if (!k) return std::nullopt;
      kids.push_back(*k);
    }
    std::size_t values = 0;
    bool variadic = false;
    for (l2::Arg a : l2::spec_of(*op).in.view()) {
      if (a == l2::Arg::value) ++values;
      if (a == l2::Arg::values) variadic = true;
    }
    if (variadic || kids.size() != values) return fail("wrong number of operands for the op"), std::nullopt;
    n.first = std::uint16_t(t.children.size());
    n.count = std::uint16_t(kids.size());
    for (auto k : kids) t.children.push_back(k);
    t.nodes.push_back(n);
    return std::uint16_t(t.nodes.size() - 1);
  }

  constexpr std::optional<ROp> rop() {
    skip();
    if (eat("?")) {
      std::string_view n = ident();
      if (n == "d") return ROp{RKind::dst, 0};
      if (n.size() == 2 && n[0] == 't' && n[1] >= '0' && n[1] <= '3') return ROp{RKind::temp, n[1] - '0'};
      for (std::size_t i = 0; i < p.var_names.size(); ++i)
        if (p.var_names[i] == n) return ROp{RKind::var, std::int64_t(i)};
      return fail("unbound variable on the right side"), std::nullopt;
    }
    if (eat("#")) {
      auto v = number(ident());
      if (!v) return fail("expected a number after `#`"), std::nullopt;
      return ROp{RKind::lit, *v};
    }
    if (eat("^")) {
      auto v = number(ident());
      if (!v || *v < 0 || *v > 1) return fail("expected ^0 or ^1"), std::nullopt;
      return ROp{RKind::succ, *v};
    }
    if (eat("-?")) {
      auto v = var(false);
      if (!v) return fail("unbound variable on the right side"), std::nullopt;
      return ROp{RKind::neg, *v};
    }
    if (eat("msb(")) {
      if (!eat("?")) return fail("expected msb(?x)"), std::nullopt;
      auto v = var(false);
      if (!v || !eat(")")) return fail("expected msb(?x)"), std::nullopt;
      return ROp{RKind::msb, *v};
    }
    std::string_view n = ident();
    if (n == "zr") return ROp{RKind::zr, 0};
    if (n == "sp") return ROp{RKind::sp, 0};
    if (auto c = cond_named(n)) return ROp{RKind::cond, std::to_underlying(*c)};
    return fail("unknown operand on the right side"), std::nullopt;
  }

  constexpr bool inst() {
    auto form = form_named(ident());
    if (!form) return fail("unknown form");
    RInst in{*form, {}};
    std::size_t want = spec_of(*form).ops.size;
    for (std::size_t i = 0; i < want; ++i) {
      auto o = rop();
      if (!o) return false;
      in.ops.at[in.ops.size++] = *o;
    }
    t.insts.push_back(in);
    return true;
  }

  constexpr bool cond() {
    std::string_view n = ident();
    constexpr std::pair<std::string_view, CKind> names[] = {
        {"uimm12", CKind::uimm12}, {"nuimm12", CKind::nuimm12}, {"uoff", CKind::uoff},
        {"nonneg", CKind::nonneg}, {"local", CKind::local},     {"extern", CKind::extern_},
        {"wide", CKind::wide},     {"narrow", CKind::narrow},   {"signed", CKind::signed_},
        {"unsigned", CKind::unsigned_}};
    for (auto [name, kind] : names) {
      if (n != name) continue;
      if (!eat("(") || !eat("?")) return fail("expected PRED(?x)");
      auto v = var(false);
      if (!v || !eat(")")) return fail("expected PRED(?x) with a bound variable");
      t.conds.push_back({kind, *v});
      return true;
    }
    return fail("unknown condition");
  }

  constexpr bool line() {
    if (ident() != "pattern") return fail("expected `pattern`");
    p.name = ident();
    if (p.name.empty() || !eat(":")) return fail("expected `pattern NAME :`");
    auto root = tree();
    if (!root) return false;
    if (t.nodes[*root].is_var) return fail("the left side must be an op");
    p.root = *root;
    if (!eat("=>")) return fail("expected `=>`");
    p.first_inst = std::uint16_t(t.insts.size());
    do {
      if (!inst()) return false;
    } while (eat(";"));
    p.ninsts = std::uint16_t(t.insts.size() - p.first_inst);
    p.cost = p.ninsts;
    p.first_cond = std::uint16_t(t.conds.size());
    if (eat("where")) {
      do {
        if (!cond()) return false;
      } while (eat(","));
    }
    p.nconds = std::uint16_t(t.conds.size() - p.first_cond);
    if (eat("cost")) {
      auto c = number(ident());
      if (!c) return fail("expected a number after `cost`");
      p.cost = std::uint16_t(*c);
    }
    skip();
    if (at != s.size()) return fail("unexpected text at the end of the pattern");
    p.nvars = std::uint8_t(p.var_names.size());
    return true;
  }
};

constexpr std::uint16_t count_nodes(const Table& t, std::uint16_t n) {
  const PNode& p = t.nodes[n];
  if (p.is_var) return 0;
  std::uint16_t c = 1;
  for (std::uint16_t i = 0; i < p.count; ++i) c += count_nodes(t, t.children[p.first + i]);
  return c;
}

export constexpr std::expected<Table, TableError> parse_patterns(std::string_view text) {
  Table t;
  std::uint32_t lineno = 0;
  while (!text.empty()) {
    ++lineno;
    std::size_t nl = text.find('\n');
    std::string_view line = text.substr(0, nl);
    text = nl == std::string_view::npos ? std::string_view{} : text.substr(nl + 1);
    if (std::size_t hash = line.find('#'); hash != std::string_view::npos) {
      // `#` starts a comment unless it is an immediate (`#N`) on the right side.
      std::size_t b = 0;
      while (b < line.size() && space(line[b])) ++b;
      if (b == hash) line = {};
    }
    std::size_t b = 0;
    while (b < line.size() && space(line[b])) ++b;
    if (b == line.size()) continue;
    Pattern p;
    LineParser lp{line, 0, t, p, {}};
    if (!lp.line()) return std::unexpected(TableError{lineno, lp.error});
    p.size = count_nodes(t, p.root);
    t.patterns.push_back(std::move(p));
  }
  return t;
}

constexpr char pattern_text[] = {
#embed "aarch64.patterns"
    , 0};

export constexpr std::string_view pattern_source{pattern_text, sizeof pattern_text - 1};

consteval bool table_parses() { return parse_patterns(pattern_source).has_value(); }
static_assert(table_parses(), "aarch64.patterns does not parse; run unit.aarch64 for the line");

export const Table& patterns() {
  static const Table t = *parse_patterns(pattern_source);
  return t;
}

}  // namespace ycxx::aarch64
