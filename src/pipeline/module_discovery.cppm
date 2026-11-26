// Discover named module declarations and imports in the selected preprocessing view (#1349).
export module ycxx.pipeline:module_discovery;

import std;
import ycxx.engine;
import ycxx.pp;
import :lines;

export namespace ycxx::pipeline {

enum class ModuleDirectiveKind { interface_unit, implementation_unit, import, exported_import };

std::string_view module_directive_name(ModuleDirectiveKind kind) {
  switch (kind) {
    case ModuleDirectiveKind::interface_unit: return "interface unit";
    case ModuleDirectiveKind::implementation_unit: return "implementation unit";
    case ModuleDirectiveKind::import: return "import";
    case ModuleDirectiveKind::exported_import: return "export import";
  }
  std::unreachable();
}

struct ModuleDirective {
  ModuleDirectiveKind kind;
  std::string name;
  std::string path;  // physical file containing the first keyword
  std::uint32_t line;
  std::uint32_t column;  // both from 1, in physical lines and byte columns
  bool operator==(const ModuleDirective&) const = default;
};

struct ModuleScan {
  std::vector<ModuleDirective> directives;
  bool complete = false;
  bool operator==(const ModuleScan&) const = default;
};

struct ModuleDirectives {
  using Key = pp::FileId;
  using Value = ModuleScan;
  static engine::query_task<Value> run(engine::Ctx& cx, Key file);
  static std::string show(const Value& scan);
  static engine::SourceSpan trace_span(const engine::Db& db, Key file) { return pp::file_span(db, file); }
};

}  // namespace ycxx::pipeline

namespace ycxx::pipeline {
namespace {

using lex::Tk;

// [module.unit], [module.import]. Stop at the next directive even if an attribute in
// this one is unterminated. A header-name, global fragment or private fragment has no name here.
std::optional<std::string> directive_operand(const pp::Region& region, std::size_t pos, bool is_import,
                                             std::string_view enclosing) {
  auto kind = [&] {
    if (pos == region.size() || region.tokens[pos].has(lex::tok_flag::directive)) return Tk::eof;
    return region.tokens[pos].kind;
  };
  auto take = [&](Tk expected) {
    if (kind() != expected) return false;
    ++pos;
    return true;
  };
  std::string name;
  auto dotted_name = [&] {
    while (kind() == Tk::identifier) {
      name += region.name(region.tokens[pos++]);
      if (kind() != Tk::period) return true;
      name += '.';
      ++pos;
    }
    return false;
  };

  if (is_import && take(Tk::colon)) {
    name = enclosing;
    name += ':';
    if (!dotted_name()) return {};
  } else {
    if (!dotted_name()) return {};
    if (!is_import && take(Tk::colon)) {
      name += ':';
      if (!dotted_name()) return {};
    }
  }

  // Attribute arguments contain balanced token sequences ([dcl.attr.grammar]).
  while (take(Tk::l_square)) {
    if (!take(Tk::l_square)) return {};
    std::vector<Tk> closing{Tk::r_square};
    while (!closing.empty()) {
      const auto token = kind();
      switch (token) {
      case Tk::eof: return {};
      case Tk::l_paren: closing.push_back(Tk::r_paren); break;
      case Tk::l_brace: closing.push_back(Tk::r_brace); break;
      case Tk::l_square: closing.push_back(Tk::r_square); break;
      case Tk::r_paren: case Tk::r_brace: case Tk::r_square:
        if (token != closing.back()) return {};
        closing.pop_back();
        break;
      default: break;
      }
      ++pos;
    }
    if (!take(Tk::r_square)) return {};
  }
  if (!take(Tk::semi)) return {};
  return name;
}

}  // namespace

engine::query_task<ModuleScan> ModuleDirectives::run(engine::Ctx& cx, Key file) {
  ModuleScan scan;
  const auto& source = cx.fetch<pp::SourceOf>(file);
  const bool disk = source.config.view == pp::View::disk;
  if (const auto* overlay = pp::host_overlay_content(source.config, source.cwd, source.path)) {
    if (!*overlay) co_return scan;
  } else if (!disk && !pp::source_content(cx, source, source.path)) {
    co_return scan;
  }
  const auto& region = co_await cx.fetch<pp::Pp>(file);
  const auto& diagnostics = co_await cx.fetch<engine::DiagsOf>(cx.cell_of<pp::Pp>(file));
  scan.complete = std::ranges::none_of(diagnostics, [](const engine::Diagnostic& d) {
    const auto kind = lex::lex_diag_of(d.id);
    return kind && lex::message_of(*kind).severity == lex::Severity::error;
  });

  std::string enclosing;
  std::map<std::uint16_t, LineTable> lines;
  for (std::size_t i = 0; i < region.size(); ++i) {
    if (!region.tokens[i].has(lex::tok_flag::directive)) continue;
    const auto site = region.site(i);
    const bool exported = region.tokens[i].kind == Tk::kw_export;
    if (exported) ++i;  // phase 4 marks both export and the following module/import keyword
    const bool is_import = region.name(region.tokens[i]) == "import";
    auto name = directive_operand(region, i + 1, is_import, enclosing);
    if (!name) continue;
    if (!is_import) enclosing = name->substr(0, name->find(':'));

    const std::string& path = region.files[site.file];
    auto table = lines.find(site.file);
    if (table == lines.end()) {
      if (disk && path == region.files.front())
        table = lines.try_emplace(site.file, cx.fetch<pp::FileText>(file)).first;
      else {
        const auto& text = pp::source_content(cx, source, path);
        if (!text) {
          scan.complete = false;
          continue;
        }
        table = lines.try_emplace(site.file, *text).first;
      }
    }
    const auto [line, column] = table->second.line_column(site.offset);
    using Kind = ModuleDirectiveKind;
    const auto kind = is_import ? (exported ? Kind::exported_import : Kind::import)
                                : (exported ? Kind::interface_unit : Kind::implementation_unit);
    scan.directives.push_back({kind, std::move(*name), path, line, column});
  }
  co_return scan;
}

std::string ModuleDirectives::show(const Value& scan) {
  std::string out = scan.complete ? "complete" : "incomplete";
  for (const auto& d : scan.directives)
    std::format_to(std::back_inserter(out), "\n{}:{}:{}: {} {}", d.path, d.line, d.column,
                   module_directive_name(d.kind), d.name);
  return out;
}

}  // namespace ycxx::pipeline
