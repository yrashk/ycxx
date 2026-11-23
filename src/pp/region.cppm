// What phase 4 produces and records (query-engine.md §B1.1, §B9; ast.md §8.1).
//
// A `FileRegion` is the value of preprocessing one file occurrence: its own tokens, references to
// the regions it includes, the macro delta it leaves, its `#pragma once` effects, its location
// tables and its diagnostics. It is a function of the file's content, its presumed name, the
// configuration, and the answers to the questions in its trace, which are exactly the reads of
// preprocessor state that its entry state answered: `macro(name)` for every identifier tested
// that the region itself had not defined or undefined, `once(file)`, the include resolutions and
// the contents of the files it read. Verifying a trace against another entry state and reusing
// the region on success is the selection of a contextual query (§A1.9) done locally until the
// engine has contextual queries (#16).
//
// A `Region` is a translation unit's tree flattened, after phases 5 and 6: what the item splitter
// and the parser read.
export module ycxx.pp:region;

import std;
import ycxx.hash;
import ycxx.lex;
import :spell;
import :files;

export namespace ycxx::pp {

// The location of a token (ast.md §8.1): the byte offset of its spelling in a file of the
// region's file table, and the innermost macro expansion that produced it (0: none).
struct TokLoc {
  u16 file = 0;
  u32 expansion = 0;
  u32 offset = 0;
  bool operator==(const TokLoc&) const = default;
};

enum class ExpansionKind : u8 { object, function, argument, stringize, paste, va_opt, pragma, builtin, embed };

// A macro expansion (ast.md §8.1). Index 0 of a table is unused, so 0 means "none".
struct Expansion {
  ExpansionKind kind = ExpansionKind::object;
  u32 macro = 0;   // the macro's name: a `Spellings` id in a FileRegion, a names index in a Region
  u32 parent = 0;  // the expansion that produced the macro's name, 0 if it was written
  // The macro name's spelling: in the file being preprocessed, or, for a nested expansion, in the
  // outer macro's definition file.
  u16 site_file = 0;
  u32 site_offset = 0;
  // The macro's name in its `#define`.
  u16 definition_file = 0;
  u32 definition_offset = 0;
  u32 operand_a = 0;
  u32 operand_b = 0;
  bool operator==(const Expansion&) const = default;
};

// A file index of a region's location table that names a binding instead of a file: the
// definition of a macro the region used but did not define, which its includer resolves. The
// offset of a location in it is relative to that definition's name.
inline constexpr u16 bound_file = 0x8000;
constexpr bool is_bound(u16 file) { return (file & bound_file) != 0; }

// A phase-4 token: payloads are `Spellings` ids; `loc.file` indexes the file table of the region
// being built, or its bindings. A painted identifier is never replaced again ([cpp.rescan]/2).
struct PTok {
  Tok tok;
  TokLoc loc;
  bool painted = false;
};

// A macro definition ([cpp.replace]).
struct MacroDef {
  u32 name = 0;
  bool function_like = false;
  bool variadic = false;
  std::vector<u32> params;  // `Spellings` ids; `__VA_ARGS__` is not listed
  // The replacement list: tokens with their spelling offsets in `file`. `space_before` records
  // whitespace before a token ([cpp.replace.general]/2 compares it), never for the first.
  std::vector<Tok> body;
  std::vector<u32> offsets;
  FileId file{};
  u32 file_name = 0;  // its presumed name, a `Spellings` id
  u32 offset = 0;     // of the name in the directive
  // The answer of `macro(name)` when this is the definition: its spelling, and its tokens' offsets
  // relative to the name, but not where it is, so a region that uses it is reused wherever an
  // equal definition is in scope (its includer binds the location).
  Fp128 fp;
  // The macros whose replacement the preprocessor computes ([cpp.predefined]): `__LINE__`,
  // `__FILE__`, `__COUNTER__`, `__DATE__`, `__TIME__`.
  enum class Dynamic : u8 { none, line, file, counter, date, time } dynamic = Dynamic::none;

  // [cpp.replace.general]/2: the same parameters and replacement list, whitespace separations
  // compared by presence.
  bool same_as(const MacroDef& o) const {
    if (function_like != o.function_like || variadic != o.variadic || params != o.params) return false;
    if (body.size() != o.body.size()) return false;
    for (std::size_t i = 0; i < body.size(); ++i) {
      const Tok& a = body[i];
      const Tok& b = o.body[i];
      if (a.kind != b.kind || a.payload != b.payload || (a.flags & tok_flag::alt) != (b.flags & tok_flag::alt)) return false;
      if (i > 0 && a.has(tok_flag::space_before) != b.has(tok_flag::space_before)) return false;
    }
    return true;
  }
};

// A question a region asked of the state at its entry, with the answer it got.
enum class Ask : u8 {
  macro,    // a: name id; answer: the definition's fp, or zero if undefined
  once,     // a: FileId; answer: 1 if it was marked `#pragma once`
  defined,  // a: name id; answer: 1 if it is a macro (`#ifdef`, `defined`)
  counter,  // answer: the value of `__COUNTER__` at entry
  time,     // answer: the fingerprint of `__DATE__` and `__TIME__`
  include,  // a: header spelling id (delimiters included); b: IncludeForm bits; c: includer FileId;
            // d: the includer's found directory; answer: the resolved file and directory, or zero
  content,  // a: FileId; answer: the content's fingerprint, or zero if missing
};
struct Question {
  Ask ask = Ask::macro;
  u32 a = 0, b = 0, c = 0, d = 0;
  Fp128 answer;
  bool operator==(const Question&) const = default;
};

struct FileRegion;

// Where the definition a region's binding names is, as the region that includes it sees it: in
// one of its files, or its own binding `binding` if it did not define the macro either.
struct Bound {
  u16 file = 0;
  u32 offset = 0;
  bool outer = false;
  u32 binding = 0;
  bool operator==(const Bound&) const = default;
};

// A child region and where it goes: before own token `at`; `offset` is the directive's.
// `bindings` resolves the child's bindings, in order.
struct Include {
  u32 at = 0;
  u32 offset = 0;
  std::shared_ptr<const FileRegion> region;
  std::vector<Bound> bindings;
};

// A directive line `-E` passes through (`#pragma`), before own token `at`.
struct Passthrough {
  u32 at = 0;
  std::vector<PTok> tokens;
};

// Whether a diagnostic located in a system header is not reported: its warnings, as Clang and GCC
// do (#229). Its errors are reported, and so is a `#warning`, which the header's author asked for
// and Clang and GCC report from a system header too.
constexpr bool hidden_in_system_header(lex::LexDiag kind) {
  return lex::message_of(kind).severity == lex::Severity::warning && kind != lex::LexDiag::warning_directive;
}

// A diagnostic of phase 4 or earlier: a byte offset in a file of the region's file table.
struct Diag {
  u16 file = 0;
  u32 offset = 0;
  lex::LexDiag kind{};
  std::array<u64, 2> args{};
  // After a #line ([cpp.line]): the presumed line, and the presumed name (a `Spellings` id) if
  // the #line gave one; 0 when there was none.
  u32 presumed_line = 0;
  std::optional<u32> presumed_name;
  bool operator==(const Diag&) const = default;
};

// The net effect of a region on one macro: its definition after the region, or none.
struct MacroChange {
  u32 name = 0;
  std::shared_ptr<const MacroDef> def;
};

struct FileRegion {
  FileId file{};
  u32 presumed = 0;  // the presumed name, a `Spellings` id
  Fp128 content;
  // Own tokens and their locations; `files[0]` is the region's own file.
  std::vector<PTok> tokens;
  std::vector<FileId> files;
  std::vector<u32> file_names;  // presumed names, parallel to `files`
  std::vector<Expansion> expansions{Expansion{}};
  // The macros whose definitions locations in `bound_file | k` refer to, by name.
  std::vector<u32> bindings;
  std::vector<Include> includes;
  std::vector<Passthrough> passthrough;
  // Function-like invocations: before own token `at`, `-E` continues the line of offset `start`
  // through that of offset `end` in the region's file, as Clang does.
  struct Continuation {
    u32 at = 0;
    u32 start = 0;
    u32 end = 0;
  };
  std::vector<Continuation> continuations;
  std::vector<Diag> diags;
  std::vector<Question> trace;
  std::vector<MacroChange> delta;
  std::vector<FileId> once;  // files this region (or a child) marked `#pragma once`
  u32 counter = 0;           // how many times it (or a child) used `__COUNTER__`
  // [cpp.line]: from offset `at` of its file on, the presumed line is the physical one plus
  // `delta`, and the presumed name `name` (a `Spellings` id) if it has one.
  struct Line {
    u32 at = 0;
    std::int32_t delta = 0;
    std::optional<u32> name;
  };
  std::vector<Line> lines;
  bool fatal = false;        // preprocessing stopped (a missing include)
  bool system = false;       // a system header's: its warnings are not reported (#229)
  u32 depth = 1;             // the deepest nesting of files below it, itself included
  // The include guard, when the file is one `#ifndef X` ... `#endif` group: included again where
  // X is defined, it is empty.
  std::optional<u32> guard;
};

// Where a root region's binding is in the TU: a file, its presumed name and the definition's
// offset.
struct RootBinding {
  FileId file{};
  u32 file_name = 0;
  u32 offset = 0;
};

// A translation unit's tree flattened, after phases 5 and 6: what the item splitter and the
// parser read.
struct Region {
  // The tokens, then one `Tk::eof`. Kinds are never pp-only. A string literal flagged
  // `tok_flag::continued` belongs to the run that starts at the nearest earlier unflagged one,
  // which holds the run's value.
  std::vector<Tok> tokens;
  // Parallel to `tokens`; the eof's offset is the main file's size.
  std::vector<TokLoc> locs;
  // Identifier and ud-suffix spellings in UTF-8, universal-character-names replaced; an
  // identifier's payload indexes them. Numbered in order of appearance.
  std::vector<std::string> names;
  // A literal's payload indexes them.
  std::vector<Literal> literals;
  // The file table `TokLoc::file` indexes: each file's path (as the session found it) and
  // presumed name; [0] is the main file. Paths, not `FileId`s: a value never depends on how a
  // session numbered its files.
  std::vector<std::string> files;
  std::vector<std::string> file_names;
  // The expansions `TokLoc::expansion` indexes; [0] is unused.
  std::vector<Expansion> expansions{Expansion{}};
  // Engine trace details, rendered while the source files of this preprocessing run are open.
  std::string error_details;
  // Token ranges where a pack pragma can change a record's layout. Layout still uses its
  // ordinary rules. ABI features that require known natural layout can reject these ranges.
  std::vector<std::pair<u32, u32>> possibly_packed;
  // First GNU visibility pragma, if any. Ordinary parsing does not apply it.
  std::optional<u32> first_visibility_pragma;
  // A Clang attribute pragma can attach ABI attributes without changing declaration tokens.
  bool has_clang_attribute_pragma = false;
  // Physical inputs in first-read order, starting with the main file. Separate from the
  // location table, which can contain presumed names and omit inputs that produced no tokens.
  std::vector<std::string> dependencies;
  bool operator==(const Region&) const = default;

  std::size_t size() const { return tokens.size(); }
  // An identifier's spelling.
  std::string_view name(Tok t) const
    pre(t.kind == Tk::identifier)
  {
    return names[t.payload];
  }
  const Literal& literal(Tok t) const
    pre(lex::is_literal(t.kind))
  {
    return literals[t.payload];
  }
  // A token's spelling: an identifier's name, a literal's spelling, a keyword's or punctuator's
  // primary spelling.
  std::string_view spelling(Tok t) const {
    if (t.kind == Tk::identifier) return names[t.payload];
    if (lex::is_literal(t.kind)) return literals[t.payload].spelling;
    return lex::spelling(t.kind);
  }
  // Where a token was written for a reader: its spelling, or, for a token a macro produced, the
  // outermost expansion's site (ast.md §8.4).
  TokLoc site(std::size_t tok) const {
    TokLoc l = locs[tok];
    while (l.expansion != 0) {
      const Expansion& e = expansions[l.expansion];
      l = TokLoc{e.site_file, e.parent, e.site_offset};
    }
    return l;
  }
};

}  // namespace ycxx::pp
