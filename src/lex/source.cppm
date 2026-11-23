// Translation phases 1 and 2 ([lex.phases]): a UTF-8 file becomes the logical text the lexer
// reads, with maps back to the phase-1 text (for raw strings, which revert phase 2) and to
// physical byte offsets (for locations).
export module ycxx.lex:source;

import std;
import ycxx.engine;
import ycxx.memory;
import :tok;
import :diag;
import :unicode;

export namespace ycxx::lex {

// Where bytes were removed: a position `pos` at or after `at` in the shorter text is `pos + shift`
// in the longer text, up to the next gap.
struct Gap {
  u32 at = 0;
  u32 shift = 0;
};

// The prepared texts can borrow immutable bytes from a mapped snapshot. Gap tables remain
// structural values so readers can use the ordinary offset lookup for either storage source.
struct PreparedSource {
  std::string phase1_owned, text_owned;
  std::string_view phase1_mapped, text_mapped;
  std::shared_ptr<const void> mapping;
  std::vector<Gap> removed, splices;

  std::string_view phase1() const { return mapping ? phase1_mapped : std::string_view(phase1_owned); }
  std::string_view text() const { return mapping ? text_mapped : std::string_view(text_owned); }
};

class Source {
public:
  // Phase 1: the file must be well-formed UTF-8 ([lex.phases]/1); each ill-formed sequence is
  // diagnosed and its bytes are kept, so the lexer sees them as stray characters. CR LF and a
  // lone CR become LF. Phase 2: a leading U+FEFF is deleted, backslash-whitespace-new-line
  // splices are deleted, and a final new-line is appended to a non-empty file that lacks one.
  explicit Source(std::string_view file, std::vector<LexDiagnostic>& diags, bool allow_snapshot = false);

  // The logical text, after phase 2.
  std::string_view text() const { return prepared_.text(); }
  // The text after phase 1, before splicing.
  std::string_view phase1() const { return prepared_.phase1(); }

  // The phase-1 offset of a logical offset: the character's own position, past any splice.
  u32 to_phase1(u32 logical) const { return logical + shift(prepared_.splices, logical); }
  // The logical offset of a phase-1 offset; a position inside a splice maps to the character
  // after it.
  u32 to_logical(u32 p1) const;
  // The physical byte offset of a logical offset.
  u32 physical(u32 logical) const { return physical_of_phase1(to_phase1(logical)); }
  u32 physical_of_phase1(u32 p1) const { return p1 + shift(prepared_.removed, p1); }

private:
  static u32 shift(const std::vector<Gap>& gaps, u32 pos) {
    auto it = std::ranges::upper_bound(gaps, pos, {}, &Gap::at);
    return it == gaps.begin() ? 0 : std::prev(it)->shift;
  }

  PreparedSource prepared_;
};

}  // namespace ycxx::lex

namespace ycxx::lex {

namespace source_detail {

constexpr std::size_t min_snapshot_source = 4 * 1024;
constexpr std::size_t max_snapshot_source = 8 * 1024 * 1024;

PreparedSource prepare(std::string_view file, std::vector<LexDiagnostic>& diags) {
  PreparedSource data;
  auto& p1_ = data.phase1_owned;
  auto& text_ = data.text_owned;
  auto& removed_ = data.removed;
  auto& splices_ = data.splices;
  p1_.reserve(file.size() + 1);
  u32 removed = 0;
  std::size_t i = 0;
  if (file.starts_with("\xEF\xBB\xBF")) {
    i = 3;
    removed = 3;
    removed_.push_back({0, removed});
  }
  while (i < file.size()) {
    unsigned char b = static_cast<unsigned char>(file[i]);
    if (b == '\r') {
      p1_ += '\n';
      if (i + 1 < file.size() && file[i + 1] == '\n') {
        ++i;
        ++removed;
        removed_.push_back({static_cast<u32>(p1_.size()), removed});
      }
      ++i;
      continue;
    }
    if (b < 0x80) {
      p1_ += static_cast<char>(b);
      ++i;
      continue;
    }
    if (auto d = unicode::decode_utf8(file.substr(i))) {
      p1_.append(file.substr(i, d->length));
      i += d->length;
      continue;
    }
    // One diagnostic for a run of ill-formed bytes.
    if (diags.empty() || diags.back().kind != LexDiag::invalid_utf8 || diags.back().args[1] != i) {
      diags.push_back({static_cast<u32>(i), LexDiag::invalid_utf8, {b, 0}});
    }
    diags.back().args[1] = i + 1;  // where the run ends, while it grows
    p1_ += static_cast<char>(b);
    ++i;
  }

  text_.reserve(p1_.size() + 1);
  u32 spliced = 0;
  for (std::size_t j = 0; j < p1_.size();) {
    if (p1_[j] == '\\') {
      std::size_t k = j + 1;
      while (k < p1_.size() && (p1_[k] == ' ' || p1_[k] == '\t' || p1_[k] == '\v' || p1_[k] == '\f')) ++k;
      if (k < p1_.size() && p1_[k] == '\n') {
        spliced += static_cast<u32>(k + 1 - j);
        splices_.push_back({static_cast<u32>(text_.size()), spliced});
        j = k + 1;
        continue;
      }
    }
    text_ += p1_[j++];
  }
  if (!text_.empty() && text_.back() != '\n') text_ += '\n';
  return data;
}

struct PreparedFile {
  PreparedSource source;
  std::vector<LexDiagnostic> diags;
};

void hash_into(Hasher& h, const PreparedFile& f) noexcept {
  hash_append(h, f.source.phase1());
  hash_append(h, f.source.text());
  hash_append(h, f.source.removed);
  hash_append(h, f.source.splices);
  hash_append(h, f.diags);
}

void serialize_into(engine::Sink& out, const PreparedFile& f) {
  engine::serialize(out, f.source.phase1());
  engine::serialize(out, f.source.text());
  engine::serialize(out, f.source.removed);
  engine::serialize(out, f.source.splices);
  engine::serialize(out, f.diags);
}

PreparedFile deserialize_from(engine::Source& in, std::type_identity<PreparedFile>) {
  PreparedFile f;
  auto& s = f.source;
  const auto text = [&in] {
    const auto bytes = in.raw(in.length());
    if (bytes.empty()) return std::string_view{};
    return std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size());
  };
  const auto p1 = text();
  const auto logical = text();
  if (in.storage()) {
    s.mapping = in.storage();
    s.phase1_mapped = p1;
    s.text_mapped = logical;
  } else {
    s.phase1_owned = p1;
    s.text_owned = logical;
  }
  s.removed = engine::deserialize<std::vector<Gap>>(in);
  s.splices = engine::deserialize<std::vector<Gap>>(in);
  f.diags = engine::deserialize<std::vector<LexDiagnostic>>(in);
  const auto valid_gaps = [](std::span<const Gap> gaps, std::size_t shorter, std::size_t longer) {
    u32 prev_at = 0;
    u32 prev_shift = 0;
    for (const Gap g : gaps) {
      if (g.at < prev_at || g.shift <= prev_shift || g.at > shorter ||
          std::uint64_t{g.at} + g.shift > longer) return false;
      prev_at = g.at;
      prev_shift = g.shift;
    }
    return true;
  };
  if (p1.size() > max_snapshot_source || logical.size() > p1.size() + 1 ||
      !valid_gaps(s.removed, p1.size(), max_snapshot_source) ||
      !valid_gaps(s.splices, logical.size(), p1.size())) in.fail();
  // These diagnostics are nested source data. Validate their enumerators before the caller
  // uses them to index the lexer's diagnostics table.
  for (const auto& d : f.diags) {
    if (d.kind != LexDiag::invalid_utf8 || d.args[0] > 255 ||
        d.offset >= d.args[1] || d.args[1] > max_snapshot_source) in.fail();
  }
  return f;
}

struct [[=engine::input{engine::Durability::low}]] FileBytes {
  using Key = u32;
  using Value = std::string_view;
};
struct PrepareFile {
  using Key = u32;
  using Value = PreparedFile;
  static engine::query_task<Value> run(engine::Ctx& cx, Key key) {
    PreparedFile out;
    out.source = prepare(cx.fetch<FileBytes>(key), out.diags);
    co_return out;
  }
};

struct SourceCache {
  std::string directory;
  Fp128 build;
};

std::optional<PreparedFile> load_or_prepare(std::string_view bytes) {
  if (bytes.size() < min_snapshot_source || bytes.size() > max_snapshot_source) return std::nullopt;
  static const auto config = [] -> std::optional<SourceCache> {
    const char* path = std::getenv("YCXX_ENGINE_SNAPSHOT_DIR");
    if (!path || path[0] != '/') return std::nullopt;
    if (const auto build = engine::snapshot_build_id()) return SourceCache{path, *build};
    return std::nullopt;
  }();
  if (!config) return std::nullopt;
  const auto content = fingerprint(bytes);
  const auto path = std::format("{}/{:016x}{:016x}-{:016x}{:016x}.source",
                                config->directory, config->build.hi, config->build.lo, content.hi, content.lo);
  // The nested DB manages its own execution target. Restore the caller's target when returning
  // the value to the outer query.
  auto* const caller = memory::target();
  memory::Target isolated(nullptr);
  engine::Db db;
  constexpr engine::q<FileBytes> input{0};
  db.set<FileBytes>(input.key, bytes);
  const bool imported = db.import_cell<PrepareFile>(path, config->build, 0, input);
  const auto& value = db.get<PrepareFile>(0);
  if (!imported || db.runs<PrepareFile>(0) != 0) (void)db.export_cell<PrepareFile>(path, config->build, 0, input);
  memory::Target retained(caller);
  return value;
}

}  // namespace source_detail

Source::Source(std::string_view file, std::vector<LexDiagnostic>& diags, bool allow_snapshot) {
  if (allow_snapshot && diags.empty()) {
    if (auto cached = source_detail::load_or_prepare(file)) {
      diags = std::move(cached->diags);
      prepared_ = std::move(cached->source);
      return;
    }
  }
  prepared_ = source_detail::prepare(file, diags);
}

u32 Source::to_logical(u32 p1) const {
  const auto& splices_ = prepared_.splices;
  // Splice k occupies phase-1 offsets [at_k + shift_{k-1}, at_k + shift_k).
  auto start = [&](std::size_t k) { return splices_[k].at + (k == 0 ? 0 : splices_[k - 1].shift); };
  std::size_t lo = 0, hi = splices_.size();
  while (lo < hi) {
    std::size_t mid = (lo + hi) / 2;
    if (start(mid) <= p1) lo = mid + 1;
    else hi = mid;
  }
  if (lo == 0) return p1;
  const Gap& g = splices_[lo - 1];
  return p1 < g.at + g.shift ? g.at : p1 - g.shift;
}

}  // namespace ycxx::lex
