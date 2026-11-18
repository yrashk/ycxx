// The textual form of L1, for tests. Printing a canonical body and parsing it back gives the same
// body; the printer names values and blocks by their dense ids, while the parser accepts any
// names and any order of blocks and nodes, which `Builder::finish` then canonicalizes.
//
//   func @7 {
//   ^0(%0: !state, %1: T3):
//     %2: T3 = add nsw %1, %1
//     %3: !state, %4: T3 = call %0, %2 [@12]
//     %5: T1 = icmp %4, %1 [slt]
//     cond_br %5 ^1(%3), ^2
//   ^1(%6: !state):
//     ret %6, %4
//   ^2:
//     unreachable
//   }
//
// A node is `results = op flags operands [immediates] successors`. Types are `!state`, `!exn`,
// `!coro` and `T<n>` (a QualType, until the AST's type printer is wired in). Immediates follow
// the op's spec: `T<n>` a type, `@<n>` an entity, `#<n>` a constant, a number, an enumerator's
// name, `/f1/b0/v2/e3` an access path (field, base, virtual base, element; `/` is empty), and
// `(#1, #2)` a list of constants. `//` starts a comment.
export module ycxx.ir:text;

import std;
import :handles;
import :ops;
import :function;
import :builder;

namespace ycxx::ir {

export struct NamedFunction {
  EntityId name{};
  Function body;
  bool operator==(const NamedFunction&) const = default;
};

export struct ParseError {
  std::uint32_t line = 0, column = 0;
  std::string message;
};

export void print(std::string& out, EntityId name, const Function& f);
export std::string print(EntityId name, const Function& f) {
  std::string s;
  print(s, name, f);
  return s;
}
export std::expected<std::vector<NamedFunction>, ParseError> parse(std::string_view text);

}  // namespace ycxx::ir

namespace ycxx::ir {

namespace {

void print_type(std::string& out, Ty t) {
  switch (t.kind) {
    case TyKind::state: out += "!state"; break;
    case TyKind::exn: out += "!exn"; break;
    case TyKind::coro: out += "!coro"; break;
    case TyKind::cxx: std::format_to(std::back_inserter(out), "T{}", raw(t.type)); break;
  }
}

constexpr char step_letter[] = {'f', 'b', 'v', 'e'};

// Prints the immediates of a node; returns nothing, consumes `words` per the spec.
void print_imms(std::string& out, Op op, std::span<const std::uint64_t> words) {
  auto kinds = spec_of(op).imm.view();
  if (kinds.empty()) return;
  out += " [";
  std::size_t at = 0;
  for (std::size_t i = 0; i < kinds.size(); ++i) {
    if (i) out += ", ";
    std::uint64_t w = words[at++];
    auto o = std::back_inserter(out);
    switch (kinds[i]) {
      case Imm::type: std::format_to(o, "T{}", w); break;
      case Imm::entity: std::format_to(o, "@{}", w); break;
      case Imm::constant: std::format_to(o, "#{}", w); break;
      case Imm::u32: std::format_to(o, "{}", w); break;
      case Imm::pred: out += name_of(Pred(w)); break;
      case Imm::storage: out += name_of(Storage(w)); break;
      case Imm::init: out += name_of(Init(w)); break;
      case Imm::order: out += name_of(MemOrder(w)); break;
      case Imm::rmw: out += name_of(Rmw(w)); break;
      case Imm::path:
        if (w == 0) out += '/';
        for (std::uint64_t j = 0; j < w; ++j) {
          PathStep s = decode_step(words[at++]);
          std::format_to(o, "/{}{}", step_letter[std::to_underlying(s.step)], s.index);
        }
        break;
      case Imm::consts:
        out += '(';
        for (std::uint64_t j = 0; j < w; ++j) std::format_to(o, "{}#{}", j ? ", " : "", words[at++]);
        out += ')';
        break;
    }
  }
  out += ']';
}

}  // namespace

void print(std::string& out, EntityId name, const Function& f) {
  auto o = std::back_inserter(out);
  std::format_to(o, "func @{} {{\n", raw(name));
  for (BlockId b : f.block_ids()) {
    std::format_to(o, "^{}", raw(b));
    if (f.block(b).nparams) {
      out += '(';
      bool first = true;
      for (ValueId p : f.params_of(b)) {
        std::format_to(o, "{}%{}: ", first ? "" : ", ", raw(p));
        print_type(out, f.value(p).type);
        first = false;
      }
      out += ')';
    }
    out += ":\n";
    for (NodeId n : f.nodes_of(b)) {
      const Node& node = f.node(n);
      out += "  ";
      bool first = true;
      for (ValueId r : f.results_of(n)) {
        std::format_to(o, "{}%{}: ", first ? "" : ", ", raw(r));
        print_type(out, f.value(r).type);
        first = false;
      }
      if (!first) out += " = ";
      out += info(node.op).name;
      template for (constexpr auto e : std::define_static_array(std::meta::enumerators_of(^^Flag)))
        if (node.flags & bit([:e:])) std::format_to(o, " {}", name_of([:e:]));
      first = true;
      for (ValueId v : f.operands_of(n)) {
        std::format_to(o, "{}%{}", first ? " " : ", ", raw(v));
        first = false;
      }
      print_imms(out, node.op, f.imms_of(n));
      first = true;
      for (const Succ& s : f.succs_of(n)) {
        std::format_to(o, "{}^{}", first ? " " : ", ", raw(s.target));
        first = false;
        if (s.nargs) {
          out += '(';
          bool a = true;
          for (ValueId v : f.args_of(s)) {
            std::format_to(o, "{}%{}", a ? "" : ", ", raw(v));
            a = false;
          }
          out += ')';
        }
      }
      out += '\n';
    }
  }
  out += "}\n";
}

namespace {

class Parser {
 public:
  explicit Parser(std::string_view text) : s_(text) {}

  std::expected<std::vector<NamedFunction>, ParseError> module() {
    std::vector<NamedFunction> out;
    skip();
    while (pos_ < s_.size()) {
      auto f = function();
      if (!f) return std::unexpected(f.error());
      out.push_back(std::move(*f));
      skip();
    }
    return out;
  }

 private:
  struct Fixup {
    NodeId node;
    std::uint32_t succ;  // none: an operand
    std::uint32_t index;
    std::string name;
    std::size_t pos;
  };

  std::string_view s_;
  std::size_t pos_ = 0;

  ParseError error_at(std::size_t pos, std::string msg) const {
    ParseError e{1, 1, std::move(msg)};
    for (std::size_t i = 0; i < pos && i < s_.size(); ++i) {
      if (s_[i] == '\n') {
        ++e.line;
        e.column = 1;
      } else {
        ++e.column;
      }
    }
    return e;
  }
  std::unexpected<ParseError> fail(std::string msg) const { return std::unexpected(error_at(pos_, std::move(msg))); }

  void skip() {
    while (pos_ < s_.size()) {
      if (std::isspace(static_cast<unsigned char>(s_[pos_]))) {
        ++pos_;
      } else if (s_.substr(pos_).starts_with("//")) {
        while (pos_ < s_.size() && s_[pos_] != '\n') ++pos_;
      } else {
        break;
      }
    }
  }
  bool peek(char c) {
    skip();
    return pos_ < s_.size() && s_[pos_] == c;
  }
  bool eat(char c) {
    if (!peek(c)) return false;
    ++pos_;
    return true;
  }
  static bool word_char(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '.'; }
  // A word: letters, digits, `_` and `.`, as op and enumerator names and value names are.
  std::string_view word() {
    skip();
    std::size_t b = pos_;
    while (pos_ < s_.size() && word_char(s_[pos_])) ++pos_;
    return s_.substr(b, pos_ - b);
  }
  std::optional<std::uint64_t> number() {
    skip();
    std::uint64_t v = 0;
    auto [p, ec] = std::from_chars(s_.data() + pos_, s_.data() + s_.size(), v);
    if (ec != std::errc{}) return std::nullopt;
    pos_ = std::size_t(p - s_.data());
    return v;
  }
  std::optional<std::uint64_t> prefixed(char c) {
    if (!eat(c)) return std::nullopt;
    if (pos_ < s_.size() && std::isspace(static_cast<unsigned char>(s_[pos_]))) return std::nullopt;
    return number();
  }

  std::expected<Ty, ParseError> type() {
    skip();
    if (eat('!')) {
      auto w = word();
      if (w == "state") return Ty::state();
      if (w == "exn") return Ty::exn();
      if (w == "coro") return Ty::coro();
      return fail(std::format("unknown IR type !{}", w));
    }
    if (auto n = prefixed('T'); n && *n <= 0xffffffffu) return Ty::of(QualType(std::uint32_t(*n)));
    return fail("expected a type");
  }

  std::expected<std::uint64_t, ParseError> handle(char prefix, std::string_view what) {
    auto n = prefixed(prefix);
    if (!n || *n > 0xffffffffu) return fail(std::format("expected {}", what));
    return *n;
  }

  template <class E>
  std::expected<std::uint64_t, ParseError> enumerator(std::string_view what) {
    auto w = word();
    if (auto e = named<E>(w)) return std::to_underlying(*e);
    return fail(std::format("expected {}, found `{}`", what, w));
  }

  std::expected<void, ParseError> imm(Imm k, std::vector<std::uint64_t>& out) {
    auto push = [&](std::expected<std::uint64_t, ParseError> v) -> std::expected<void, ParseError> {
      if (!v) return std::unexpected(v.error());
      out.push_back(*v);
      return {};
    };
    switch (k) {
      case Imm::type: return push(handle('T', "a type"));
      case Imm::entity: return push(handle('@', "an entity"));
      case Imm::constant: return push(handle('#', "a constant"));
      case Imm::u32: {
        auto n = number();
        if (!n || *n > 0xffffffffu) return fail("expected a number");
        return push(*n);
      }
      case Imm::pred: return push(enumerator<Pred>("a predicate"));
      case Imm::storage: return push(enumerator<Storage>("a storage duration"));
      case Imm::init: return push(enumerator<Init>("an init kind"));
      case Imm::order: return push(enumerator<MemOrder>("a memory order"));
      case Imm::rmw: return push(enumerator<Rmw>("an rmw operation"));
      case Imm::path: {
        if (!peek('/')) return fail("expected an access path");
        std::size_t count_at = out.size();
        out.push_back(0);
        while (eat('/')) {
          if (pos_ >= s_.size()) break;
          auto letter = std::ranges::find(step_letter, s_[pos_]);
          if (letter == std::end(step_letter)) {
            if (out.size() == count_at + 1) break;  // `/` alone: the empty path
            return fail("expected a path step");
          }
          ++pos_;
          auto n = number();
          if (!n || *n > 0xffffffffu) return fail("expected a path step index");
          out.push_back(encode({Step(letter - std::begin(step_letter)), std::uint32_t(*n)}));
          ++out[count_at];
        }
        return {};
      }
      case Imm::consts: {
        if (!eat('(')) return fail("expected `(`");
        std::size_t count_at = out.size();
        out.push_back(0);
        if (!eat(')')) {
          do {
            if (auto r = push(handle('#', "a constant")); !r) return r;
            ++out[count_at];
          } while (eat(','));
          if (!eat(')')) return fail("expected `)`");
        }
        return {};
      }
    }
    return fail("unknown immediate kind");
  }

  // `%name`
  std::expected<std::string, ParseError> value_name() {
    if (!eat('%')) return fail("expected a value");
    auto w = word();
    if (w.empty()) return fail("expected a value name");
    return std::string(w);
  }

  // Lookahead: `^name` starting a block (`^name:` or `^name(%x: T):`) rather than a successor.
  bool at_block_label() {
    skip();
    std::size_t save = pos_;
    bool label = false;
    if (eat('^')) {
      word();
      if (peek(':')) {
        label = true;
      } else if (eat('(')) {
        if (peek(')')) {
          ++pos_;
          label = peek(':');
        } else if (eat('%')) {
          word();
          label = peek(':');
        }
      }
    }
    pos_ = save;
    return label;
  }

  std::expected<NamedFunction, ParseError> function() {
    if (word() != "func") return fail("expected `func`");
    auto name = handle('@', "the function's entity");
    if (!name) return std::unexpected(name.error());
    std::size_t header = pos_;
    if (!eat('{')) return fail("expected `{`");

    Builder b;
    std::map<std::string, ValueId, std::less<>> values;
    std::map<std::string, BlockId, std::less<>> blocks;
    std::vector<Fixup> fixups;
    struct Target { NodeId node; std::uint32_t succ; std::string name; std::size_t pos; };
    std::vector<Target> targets;

    auto define = [&](const std::string& n, ValueId v) -> std::expected<void, ParseError> {
      if (!values.emplace(n, v).second) return fail(std::format("%{} is defined twice", n));
      return {};
    };

    while (!eat('}')) {
      if (!eat('^')) return fail("expected a block label or `}`");
      std::string label(word());
      std::vector<Ty> ptypes;
      std::vector<std::string> pnames;
      if (eat('(')) {
        if (!eat(')')) {
          do {
            auto n = value_name();
            if (!n) return std::unexpected(n.error());
            if (!eat(':')) return fail("expected `:`");
            auto t = type();
            if (!t) return std::unexpected(t.error());
            pnames.push_back(*n);
            ptypes.push_back(*t);
          } while (eat(','));
          if (!eat(')')) return fail("expected `)`");
        }
      }
      if (!eat(':')) return fail("expected `:` after the block label");
      BlockId blk = b.add_block(std::span<const Ty>(ptypes));
      if (!blocks.emplace(label, blk).second) return fail(std::format("block ^{} is defined twice", label));
      for (std::uint32_t i = 0; i < pnames.size(); ++i)
        if (auto r = define(pnames[i], b.param(blk, i)); !r) return std::unexpected(r.error());

      // Nodes, up to and including the terminator.
      for (bool ended = false; !ended;) {
        std::vector<std::string> rnames;
        std::vector<Ty> rtypes;
        if (peek('%')) {
          do {
            auto n = value_name();
            if (!n) return std::unexpected(n.error());
            if (!eat(':')) return fail("expected `:`");
            auto t = type();
            if (!t) return std::unexpected(t.error());
            rnames.push_back(*n);
            rtypes.push_back(*t);
          } while (eat(','));
          if (!eat('=')) return fail("expected `=`");
        }
        std::size_t op_pos = (skip(), pos_);
        auto opname = word();
        auto op = op_named(opname);
        if (!op) return std::unexpected(error_at(op_pos, std::format("unknown op `{}`", opname)));
        const OpSpec& sp = spec_of(*op);

        Flags flags = 0;
        for (;;) {
          std::size_t save = pos_;
          auto w = word();
          auto fl = named<Flag>(w);
          if (w.empty() || !fl) {
            pos_ = save;
            break;
          }
          flags |= bit(*fl);
        }

        std::vector<std::pair<std::string, std::size_t>> opnds;
        if (!sp.in.view().empty() && peek('%')) {
          do {
            std::size_t at = (skip(), pos_);
            auto n = value_name();
            if (!n) return std::unexpected(n.error());
            opnds.emplace_back(*n, at);
          } while (eat(','));
        }

        std::vector<std::uint64_t> imms;
        if (!sp.imm.view().empty()) {
          if (!eat('[')) return fail(std::format("`{}` needs immediates in `[...]`", opname));
          bool first = true;
          for (Imm k : sp.imm.view()) {
            if (!first && !eat(',')) return fail("expected `,`");
            first = false;
            if (auto r = imm(k, imms); !r) return std::unexpected(r.error());
          }
          if (!eat(']')) return fail("expected `]`");
        }

        struct S { std::string target; std::size_t pos; std::vector<std::pair<std::string, std::size_t>> args; };
        std::vector<S> succs;
        if (sp.cls == NodeClass::control) {
          while (peek('^') && !at_block_label()) {
            S& e = succs.emplace_back();
            e.pos = pos_;
            eat('^');
            e.target = std::string(word());
            if (eat('(')) {
              if (!eat(')')) {
                do {
                  std::size_t at = (skip(), pos_);
                  auto n = value_name();
                  if (!n) return std::unexpected(n.error());
                  e.args.emplace_back(*n, at);
                } while (eat(','));
                if (!eat(')')) return fail("expected `)`");
              }
            }
            if (!eat(',')) break;
          }
          ended = true;
        }

        std::vector<ValueId> ops(opnds.size(), ValueId(none));
        std::vector<SuccRef> refs(succs.size());
        for (std::size_t i = 0; i < succs.size(); ++i)
          refs[i] = {BlockId(none), std::vector<ValueId>(succs[i].args.size(), ValueId(none))};
        NodeId n = b.emit(blk, *op, flags, std::span<const Ty>(rtypes), std::span<const ValueId>(ops),
                          std::span<const std::uint64_t>(imms), std::span<const SuccRef>(refs));
        for (std::uint32_t i = 0; i < opnds.size(); ++i)
          fixups.push_back({n, none, i, opnds[i].first, opnds[i].second});
        for (std::uint32_t s = 0; s < succs.size(); ++s) {
          targets.push_back({n, s, succs[s].target, succs[s].pos});
          for (std::uint32_t i = 0; i < succs[s].args.size(); ++i)
            fixups.push_back({n, s, i, succs[s].args[i].first, succs[s].args[i].second});
        }
        for (std::uint32_t i = 0; i < rnames.size(); ++i)
          if (auto r = define(rnames[i], b.result(n, i)); !r) return std::unexpected(r.error());
        if (!ended && peek('^')) return fail("a block must end with a terminator");
        if (!ended && peek('}')) return fail("a block must end with a terminator");
      }
    }

    for (const Fixup& x : fixups) {
      auto it = values.find(x.name);
      if (it == values.end()) return std::unexpected(error_at(x.pos, std::format("%{} is not defined", x.name)));
      if (x.succ == none) b.set_operand(x.node, x.index, it->second);
      else b.set_arg(x.node, x.succ, x.index, it->second);
    }
    for (const Target& t : targets) {
      auto it = blocks.find(t.name);
      if (it == blocks.end()) return std::unexpected(error_at(t.pos, std::format("block ^{} is not defined", t.name)));
      b.set_target(t.node, t.succ, it->second);
    }
    auto built = b.finish();
    if (!built) return std::unexpected(error_at(header, built.error()));
    if (auto v = verify(built->function); !v) return std::unexpected(error_at(header, v.error()));
    return NamedFunction{EntityId(std::uint32_t(*name)), std::move(built->function)};
  }
};

}  // namespace

std::expected<std::vector<NamedFunction>, ParseError> parse(std::string_view text) {
  return Parser(text).module();
}

}  // namespace ycxx::ir
