// Lines and columns of byte offsets, for rendering diagnostics as `path:line:col:` (ast.md §8.3,
// step 4). A minimal table over one file's text: #34 owns the full one (presumed lines, macro
// expansions, `#line`), which replaces `LineTable` behind `line_column`.
export module ycxx.pipeline:lines;

import std;

export namespace ycxx::pipeline {

struct LineColumn {
  std::uint32_t line = 1;    // from 1
  std::uint32_t column = 1;  // from 1, in bytes
  bool operator==(const LineColumn&) const = default;
};

class LineTable {
 public:
  // Lines end at LF, CR LF or a lone CR ([lex.phases]/1 maps both to new-lines).
  explicit LineTable(std::string_view text) : size_(static_cast<std::uint32_t>(text.size())) {
    starts_.push_back(0);
    for (std::uint32_t i = 0; i < size_; ++i) {
      if (text[i] == '\r' && i + 1 < size_ && text[i + 1] == '\n') ++i;
      if (text[i] == '\n' || text[i] == '\r') starts_.push_back(i + 1);
    }
  }

  // An offset past the end is the end of the file.
  // The byte offset of a 1-based line and byte column, clamped to the text: the inverse of
  // `line_column`.
  std::uint32_t offset_of(std::uint32_t line, std::uint32_t column) const {
    const auto l = std::clamp<std::size_t>(line, 1, starts_.size()) - 1;
    return std::min(size_, starts_[l] + (column ? column - 1 : 0));
  }
  LineColumn line_column(std::uint32_t offset) const {
    offset = std::min(offset, size_);
    auto it = std::ranges::upper_bound(starts_, offset);
    auto line = static_cast<std::uint32_t>(it - starts_.begin());
    return {line, offset - starts_[line - 1] + 1};
  }

 private:
  std::uint32_t size_;
  std::vector<std::uint32_t> starts_;  // the offset each line starts at
};

}  // namespace ycxx::pipeline
