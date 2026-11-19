// Lines and columns of byte offsets, for rendering diagnostics as `path:line:col:` (ast.md §8.3,
// step 4). A minimal table over one file's text: #34 owns the full one (presumed lines, macro
// expansions, `#line`), which replaces `LineTable` behind `line_column`.
export module ycxx.pipeline:lines;

import std;
import ycxx.ast;
import ycxx.engine;
import ycxx.items;
import ycxx.pp;

export namespace ycxx::pipeline {

struct LineColumn {
  std::uint32_t line = 1;    // from 1
  std::uint32_t column = 1;  // from 1, in bytes
  bool operator==(const LineColumn&) const = default;
};

class LineTable {
 public:
  // Lines end at LF, CR LF or a lone CR ([lex.phases]/1 maps both to new-lines).
  explicit LineTable(std::string_view text) : size_(static_cast<std::uint32_t>(text.size())), starts_(pp::line_starts(text)) {}

  // An offset past the end is the end of the file.
  LineColumn line_column(std::uint32_t offset) const {
    offset = std::min(offset, size_);
    auto it = std::ranges::upper_bound(starts_, offset);
    auto line = static_cast<std::uint32_t>(it - starts_.begin());
    return {line, offset - starts_[line - 1] + 1};
  }
  // The byte offset of a 1-based line and byte column, clamped to the text: the inverse of
  // `line_column`.
  std::uint32_t offset_of(std::uint32_t line, std::uint32_t column) const {
    const auto l = std::clamp<std::size_t>(line, 1, starts_.size()) - 1;
    return std::min(size_, starts_[l] + (column ? column - 1 : 0));
  }

 private:
  std::uint32_t size_;
  std::vector<std::uint32_t> starts_;  // the offset each line starts at
};

// The file and byte offset of an item location (ast.md §8.3, steps 1–3).
std::optional<pp::TokLoc> source_site(ast::Interners& in, const pp::Region& region, const items::Items& items, ast::Loc at);

// File contents as this revision sees them. An open buffer takes precedence in the editor view.
const std::optional<std::string>& source_content(engine::Ctx& cx, const pp::Source& source, std::string_view name);

struct SourcePosition {
  std::uint32_t offset;
  std::string where;  // file:line:column
};

// Renders positions against the same source inputs as preprocessing. The table for a file is
// built on its first diagnostic, then used for every diagnostic in that file.
class SourceLocations {
 public:
  SourceLocations(engine::Ctx& cx, const pp::Source& source, pp::FileId file, const pp::Region& region, const items::Items& items)
      : cx_(cx), source_(source), file_(file), region_(region), items_(items) {}

  std::string at(pp::TokLoc loc);
  std::optional<SourcePosition> at(ast::Interners& in, ast::Loc loc) {
    if (const auto site = source_site(in, region_, items_, loc)) return SourcePosition{site->offset, at(*site)};
    return std::nullopt;
  }

 private:
  engine::Ctx& cx_;
  const pp::Source& source_;
  pp::FileId file_;
  const pp::Region& region_;
  const items::Items& items_;
  std::map<std::uint16_t, LineTable> lines_;
};

}  // namespace ycxx::pipeline

namespace ycxx::pipeline {

std::optional<pp::TokLoc> source_site(ast::Interners& in, const pp::Region& region, const items::Items& items, ast::Loc at) {
  for (const items::Item& it : items.items) {
    if (in.items.add<ast::ItemId>(it.id) != at.item) continue;
    std::uint32_t tok = std::min<std::uint32_t>(it.begin + at.offset, static_cast<std::uint32_t>(region.locs.size() - 1));
    return region.site(tok);
  }
  return std::nullopt;
}

const std::optional<std::string>& source_content(engine::Ctx& cx, const pp::Source& source, std::string_view name) {
  std::filesystem::path p(name);
  if (p.is_relative() && !source.cwd.empty()) p = std::filesystem::path(source.cwd) / p;
  const pp::PathId id = pp::path_id(p.lexically_normal().generic_native_encoded_string());
  if (source.config.view == pp::View::editor)
    if (const auto& open = cx.fetch<pp::Buffer>(id)) return open;
  return cx.fetch<pp::FileContent>(id);
}

std::string SourceLocations::at(pp::TokLoc loc) {
  if (loc.file >= region_.files.size()) return {};
  const bool main = loc.file == 0;
  const std::string& name = main ? source_.path : region_.files[loc.file];
  auto it = lines_.find(loc.file);
  if (it == lines_.end()) {
    if (main && source_.config.view == pp::View::disk)
      it = lines_.try_emplace(loc.file, cx_.fetch<pp::FileText>(file_)).first;
    else if (!main && name == "<built-in>") {
      pp::Session& ps = pp::session_for(source_.config);
      std::lock_guard lock(ps.mutex);
      it = lines_.try_emplace(loc.file, ps.builtin_text()).first;
    } else {
      const auto& text = source_content(cx_, source_, name);
      if (!text) return {};
      it = lines_.try_emplace(loc.file, *text).first;
    }
  }
  const LineColumn lc = it->second.line_column(loc.offset);
  return std::format("{}:{}:{}", main ? name : region_.file_names[loc.file], lc.line, lc.column);
}

}  // namespace ycxx::pipeline
