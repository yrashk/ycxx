// Translation phases 1 and 2 ([lex.phases]): a UTF-8 file becomes the logical text the lexer
// reads, with maps back to the phase-1 text (for raw strings, which revert phase 2) and to
// physical byte offsets (for locations).
export module ycxx.lex:source;

import std;
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

class Source {
public:
  // Phase 1: the file must be well-formed UTF-8 ([lex.phases]/1); each ill-formed sequence is
  // diagnosed and its bytes are kept, so the lexer sees them as stray characters. CR LF and a
  // lone CR become LF. Phase 2: a leading U+FEFF is deleted, backslash-whitespace-new-line
  // splices are deleted, and a final new-line is appended to a non-empty file that lacks one.
  explicit Source(std::string_view file, std::vector<LexDiagnostic>& diags);

  // The logical text, after phase 2.
  std::string_view text() const { return text_; }
  // The text after phase 1, before splicing.
  std::string_view phase1() const { return p1_; }

  // The phase-1 offset of a logical offset: the character's own position, past any splice.
  u32 to_phase1(u32 logical) const { return logical + shift(splices_, logical); }
  // The logical offset of a phase-1 offset; a position inside a splice maps to the character
  // after it.
  u32 to_logical(u32 p1) const;
  // The physical byte offset of a logical offset.
  u32 physical(u32 logical) const { return physical_of_phase1(to_phase1(logical)); }
  u32 physical_of_phase1(u32 p1) const { return p1 + shift(removed_, p1); }

private:
  static u32 shift(const std::vector<Gap>& gaps, u32 pos) {
    auto it = std::ranges::upper_bound(gaps, pos, {}, &Gap::at);
    return it == gaps.begin() ? 0 : std::prev(it)->shift;
  }

  std::string p1_;
  std::string text_;
  std::vector<Gap> removed_;  // phase 1 and the BOM: phase-1 offset -> physical
  std::vector<Gap> splices_;  // phase 2: logical offset -> phase-1 offset
};

Source::Source(std::string_view file, std::vector<LexDiagnostic>& diags) {
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
}

u32 Source::to_logical(u32 p1) const {
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
