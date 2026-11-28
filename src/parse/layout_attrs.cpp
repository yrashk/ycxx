// Module implementation unit for ycxx.parse layout-attribute helpers.
// Keeping non-template bodies out of parse.cppm avoids regenerating the ycxx.parse BMI
// and recompiling 25+ downstream modules on routine edits.
module ycxx.parse;

import std;
import ycxx.lex;
import ycxx.pp;

namespace ycxx::parse {

bool layout_attribute(const pp::Region& region, u32 begin, u32 end, LayoutAttributes mode,
                      std::span<const std::pair<u32, u32>> applied,
                      std::string_view standard_attribute, bool applied_overlap) {
  using enum lex::Tk;
  for (std::uint32_t i = begin; i < end; ++i) {
    while (!applied.empty() && applied.front().second <= i) applied = applied.subspan(1);
    if (!applied.empty() && applied.front().first <= i) {
      i = applied.front().second - 1;
      continue;
    }
    auto tok = region.tokens[i];
    if (tok.kind == kw_alignas && standard_attribute.empty()) return true;
    if (i + 1 == end) break;
    const auto next = region.tokens[i + 1].kind;
    bool bracket = tok.kind == l_square && next == l_square;
    bool gnu = tok.kind == identifier && next == l_paren && region.name(tok) == "__attribute__";
    if (!bracket && !gnu) {
      if (mode == LayoutAttributes::leading_affecting_layout) return false;
      continue;
    }
    // An annotation payload does not participate in layout, even if it names packed or aligned.
    const bool annotation = bracket && i + 2 < end && region.tokens[i + 2].kind == equal;
    if (mode == LayoutAttributes::any && !annotation) return true;
    const bool unscoped = (!standard_attribute.empty() || applied_overlap) && bracket && i + 2 < end &&
                          region.tokens[i + 2].kind != kw_using;
    if (gnu) ++i;
    const auto open = bracket ? l_square : l_paren;
    const auto close = bracket ? r_square : r_paren;
    std::uint32_t depth = 1;
    std::uint32_t parentheses = 0;
    while (++i < end && depth) {
      auto part = region.tokens[i];
      if (part.kind == open) ++depth;
      else if (part.kind == close && --depth == 0) break;
      else if (unscoped && part.kind == l_paren) ++parentheses;
      else if (unscoped && part.kind == r_paren && parentheses) --parentheses;
      else if (part.kind == identifier && !annotation) {
        auto name = region.name(part);
        if (unscoped) {
          const auto previous = region.tokens[i - 1].kind;
          const auto following = i + 1 < end ? region.tokens[i + 1].kind : eof;
          if (depth == 2 && parentheses == 0 &&
              (previous == l_square || previous == comma) &&
              (following == r_square || following == comma)) {
            if (name == standard_attribute) return true;
            if (applied_overlap && name == "no_unique_address") continue;
          }
        }
        if (!standard_attribute.empty()) continue;
        if (name.size() > 4 && name.starts_with("__") && name.ends_with("__"))
          name = name.substr(2, name.size() - 4);
        if (mode == LayoutAttributes::trailing_definition && gnu && depth == 2 &&
            (region.tokens[i - 1].kind == l_paren || region.tokens[i - 1].kind == comma) &&
            (name == "copy" || name == "trivial_abi")) return true;
        if (name == "packed" || name == "aligned" || name == "mode" || name == "vector_size" ||
            name == "no_unique_address" || name == "ms_struct" || name == "gcc_struct") return true;
      }
    }
  }
  return false;
}

}  // namespace ycxx::parse
