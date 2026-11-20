// Spellings of phase 4: identifiers, header-names and literals are interned once per session, so
// a token's payload means the same in every region of every translation unit, and a region
// reused in another TU needs no renumbering. A `Region` (the parser's view) numbers its own names
// and literals in order of appearance, so its value does not depend on this table.
export module ycxx.pp:spell;

import std;
import ycxx.lex;

export namespace ycxx::pp {

using lex::Literal;
using lex::Tk;
using lex::Tok;
using lex::u16;
using lex::u32;
using lex::u64;
using lex::u8;
namespace tok_flag = lex::tok_flag;

class Spellings {
public:
  // An identifier's or header-name's spelling.
  u32 name(std::string_view s) {
    if (auto it = names_.find(s); it != names_.end()) return it->second;
    u32 id = static_cast<u32>(name_text_.size());
    name_text_.emplace_back(s);
    names_.emplace(name_text_.back(), id);
    return id;
  }
  std::string_view name(u32 id) const { return name_text_[id]; }

  // Byte strings are interned as-is, separately identified by Tk::embed.
  u32 resource(std::string_view bytes) { return name(bytes); }
  std::string_view resource(u32 id) const { return name(id); }

  // A literal or pp-number, as the lexer recorded it (spelling, ud-suffix as a name id, encoding,
  // raw, bad); keyed by its spelling, of which those are functions.
  u32 literal(Literal l) {
    if (auto it = literals_.find(l.spelling); it != literals_.end()) return it->second;
    u32 id = static_cast<u32>(literal_rec_.size());
    literal_rec_.push_back(std::move(l));
    literals_.emplace(literal_rec_.back().spelling, id);
    return id;
  }
  const Literal& literal(u32 id) const { return literal_rec_[id]; }

private:
  struct string_hash {
    using is_transparent = void;
    std::size_t operator()(std::string_view s) const noexcept { return std::hash<std::string_view>{}(s); }
  };
  // Deques: the maps' keys view the stored strings.
  std::deque<std::string> name_text_;
  std::unordered_map<std::string_view, u32, string_hash, std::equal_to<>> names_;
  std::deque<Literal> literal_rec_;
  std::unordered_map<std::string_view, u32, string_hash, std::equal_to<>> literals_;
};

// The alternative spelling of a kind flagged `tok_flag::alt` ([lex.digraph]): each kind has one.
constexpr std::string_view alt_spelling(Tk k) {
  switch (k) {
  case Tk::l_brace: return "<%";
  case Tk::r_brace: return "%>";
  case Tk::l_square: return "<:";
  case Tk::r_square: return ":>";
  case Tk::hash: return "%:";
  case Tk::hashhash: return "%:%:";
  case Tk::ampamp: return "and";
  case Tk::ampequal: return "and_eq";
  case Tk::amp: return "bitand";
  case Tk::pipe: return "bitor";
  case Tk::tilde: return "compl";
  case Tk::exclaim: return "not";
  case Tk::exclaimequal: return "not_eq";
  case Tk::pipepipe: return "or";
  case Tk::pipeequal: return "or_eq";
  case Tk::caret: return "xor";
  case Tk::caretequal: return "xor_eq";
  default: return lex::spelling(k);
  }
}

// A phase-4 token's spelling: payloads index `s` (a `pp_other` holds its code point).
std::string spelling(Tok t, const Spellings& s) {
  switch (t.kind) {
  case Tk::identifier:
  case Tk::header_name: return std::string(s.name(t.payload));
  case Tk::pp_number:
  case Tk::int_lit:
  case Tk::float_lit:
  case Tk::char_lit:
  case Tk::string_lit: return s.literal(t.payload).spelling;
  case Tk::pp_other: {
    std::string out;
    lex::unicode::append_utf8(out, char32_t(t.payload));
    return out;
  }
  case Tk::eof: return {};
  default: return std::string(t.has(tok_flag::alt) ? alt_spelling(t.kind) : lex::spelling(t.kind));
  }
}

}  // namespace ycxx::pp
