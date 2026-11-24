// Standalone, source-linked build inspection report (#1256).
export module ycxx.build.inspection_html;

import std;
import ycxx.build.inspection_model;

namespace ycxx::build {
namespace {

constexpr std::string_view styles = R"HTML(
<style>
  :root {
    color-scheme: light;
    font: 15px/1.55 system-ui, sans-serif;
    color: #172335;
    background: #f2f5f8;
    --border: #dbe2ea;
    --muted: #516078;
    --link: #175c6d;
    --mono: ui-monospace, SFMono-Regular, Consolas, monospace;
  }
  * { box-sizing: border-box; }
  body { margin: 0; }
  main { max-width: 92rem; margin: 0 auto; padding: clamp(1rem, 4vw, 3.5rem); }
  h1, h2, p, pre { margin: 0; }
  h1 { margin-bottom: 1.5rem; font-size: clamp(1.75rem, 4vw, 2.6rem); line-height: 1.12;
    letter-spacing: -.035em; overflow-wrap: anywhere; }
  h1 code { font-size: .9em; color: var(--link); }
  h2 { font-size: 1.12rem; font-weight: 680; }
  code { font-family: var(--mono); font-size: .9em; unicode-bidi: isolate; }
  a { color: var(--link); text-underline-offset: .19em; }
  a:focus-visible, .source-scroll:focus-visible { outline: 2px solid #188496; outline-offset: 3px; }
  .layout { display: grid; grid-template-columns: minmax(0, 1.45fr) minmax(0, 1fr); gap: 1.25rem; align-items: start; }
  .results { min-width: 0; display: grid; gap: 1.25rem; }
  .panel { min-width: 0; background: #fff; border: 1px solid var(--border); border-radius: .8rem; }
  .panel h2 { border-bottom: 1px solid var(--border); padding: .95rem 1.1rem; }
  .facts { width: 100%; table-layout: fixed; border-spacing: 0; overflow-wrap: anywhere; }
  .facts tr + tr > *, .facts tbody tr:first-child > * { border-top: 1px solid #e8edf2; }
  .facts th, .facts td { padding: .6rem 1.1rem; vertical-align: top; text-align: left; }
  .facts th { color: var(--muted); font-size: .76rem; font-weight: 670; }
  .facts th:first-child { width: 22%; }
  .facts th:last-child { width: 38%; }
  .owner, .fact-name, .location { display: block; }
  .owner, .origin, .fact-name { color: var(--muted); font-size: .86rem; }
  .fact-value, .fact-name, .origin { white-space: pre-wrap; overflow-wrap: anywhere; }
  .location { font: .78rem/1.55 var(--mono); }
  .changes { list-style: none; margin: 0; padding: 0; }
  .changes li { display: grid; grid-template-columns: minmax(0, 22%) minmax(0, 1fr);
    gap: 1rem; padding: .8rem 1.1rem; overflow-wrap: anywhere; }
  .changes li + li { border-top: 1px solid #e8edf2; }
  .action { display: inline-block; font-size: .79rem; font-weight: 750; }
  .added { color: #116749; }
  .removed { color: #a0333a; }
  .changed { color: #785515; }
  .before, .after { display: grid; grid-template-columns: 3.15rem minmax(0, 1fr); gap: .55rem; align-items: baseline; }
  .before > span, .after > span { color: var(--muted); font-size: .78rem; }
  .before code { color: #a0333a; }
  .after code { color: #116749; }
  .empty { padding: .9rem 1.1rem; color: var(--muted); }
  .manifest { position: sticky; top: 1rem; background: #182537; color: #ecf4ff; border-color: #24374e; }
  .manifest h2 { border-color: #34475d; }
  .source-scroll { overflow: auto; max-height: calc(100vh - 8rem); border-radius: 0 0 .75rem .75rem; }
  pre { min-width: max-content; padding: .7rem 0; font: .83rem/1.6 var(--mono); tab-size: 4; }
  pre code { font-size: 1em; }
  .source-line { display: inline-block; min-width: 100%; padding-right: 1rem; scroll-margin-block: .65rem; }
  .source-line::before { content: attr(data-number); display: inline-block; width: 3.3em; margin-right: 1em;
    text-align: right; color: #a5b6c9; user-select: none; }
  .source-line:target { background: #2d4b5e; box-shadow: inset .18rem 0 #8cdde0; }
  @media (max-width: 56rem) {
    .layout { grid-template-columns: minmax(0, 1fr); }
    .manifest { position: static; }
    .source-scroll { max-height: 65vh; }
  }
  @media (max-width: 31rem) {
    .facts th, .facts td, .changes li { padding-inline: .6rem; }
    .facts th:first-child { width: 24%; }
  }
</style>
)HTML";

// Markup syntax and whitespace get character references. Render other control characters
// as visible escapes so they cannot disappear inside a path or a command.
void escaped(std::string& out, std::string_view text) {
  for (std::size_t i = 0; i < text.size(); ++i) {
    const auto c = static_cast<unsigned char>(text[i]);
    switch (c) {
      case '&': out += "&amp;"; break;
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      case '"': out += "&quot;"; break;
      case '\'': out += "&#39;"; break;
      case '\t': out += "&#9;"; break;
      case '\n': out += "&#10;"; break;
      case '\r': out += "&#13;"; break;
      default:
        if (c < 0x20 || c == 0x7f) {
          std::format_to(std::back_inserter(out), "\\x{:02X}", static_cast<unsigned>(c));
        } else if (c == 0xc2 && i + 1 < text.size() &&
                   static_cast<unsigned char>(text[i + 1]) >= 0x80 &&
                   static_cast<unsigned char>(text[i + 1]) <= 0x9f) {
          std::format_to(std::back_inserter(out), "\\u00{:02X}", static_cast<unsigned>(static_cast<unsigned char>(text[++i])));
        } else {
          out += text[i];
        }
    }
  }
}

void append_location(std::string& out, const PlanFact& fact, std::size_t lines) {
  if (!fact.origin.empty()) {
    out += R"(<span class="origin">)";
    escaped(out, fact.origin);
    out += "</span>";
  }
  if (fact.line == 0) return;
  out += R"(<span class="location">)";
  const bool link = fact.line <= lines;
  auto next = std::back_inserter(out);
  if (link) std::format_to(next, R"(<a href="#manifest-line-{}">)", fact.line);
  std::format_to(next, "build.ycxx:{}", fact.line);
  if (fact.column != 0) std::format_to(next, ":{}", fact.column);
  if (link) out += "</a>";
  out += "</span>";
}

void append_fact_value(std::string& out, const PlanFact& fact, bool show_name = true) {
  if (show_name && !fact.name.empty() && fact.name != fact.value) {
    out += R"(<code class="fact-name">)";
    escaped(out, fact.name);
    out += "</code>";
  }
  out += R"(<code class="fact-value">)";
  if (fact.value.empty()) out += "&quot;&quot;";
  else escaped(out, fact.value);
  out += "</code>";
}

void append_owner(std::string& out, const PlanFact& fact) {
  out += R"(<span class="owner"><code>)";
  escaped(out, fact.target);
  out += "</code> · ";
  escaped(out, fact.kind);
  out += "</span>";
}

void append_changes(std::string& out, const std::vector<PlanChange>& changes, std::size_t lines) {
  out += R"(<section class="panel" aria-labelledby="changes"><h2 id="changes">Plan changes</h2>)";
  if (changes.empty()) {
    out += R"(<p class="empty">No changes.</p></section>)";
    return;
  }
  out += R"(<ul class="changes" role="list">)";
  for (const PlanChange& change : changes) {
    if (!change.before && !change.after) continue;
    const PlanFact& fact = change.after ? *change.after : *change.before;
    out += "<li><div>";
    if (change.change == "added") out += R"(<span class="action added">Added</span>)";
    else if (change.change == "removed") out += R"(<span class="action removed">Removed</span>)";
    else out += R"(<span class="action changed">Changed</span>)";
    append_owner(out, fact);
    out += "</div><div>";
    if (change.before && change.after) {
      if (!fact.name.empty() && fact.name != fact.value) {
        out += R"(<code class="fact-name">)";
        escaped(out, fact.name);
        out += "</code>";
      }
      out += R"(<div class="before"><span>Before</span><del>)";
      append_fact_value(out, *change.before, false);
      out += R"(</del></div><div class="after"><span>After</span><ins>)";
      append_fact_value(out, *change.after, false);
      out += "</ins></div>";
    } else {
      append_fact_value(out, fact);
    }
    append_location(out, fact, change.after ? lines : 0);
    out += "</div></li>\n";
  }
  out += "</ul></section>\n";
}

void append_facts(std::string& out, const std::vector<PlanFact>& facts, std::size_t lines) {
  out += R"(<section class="panel" aria-labelledby="facts"><h2 id="facts">Resolved facts</h2>)";
  if (facts.empty()) {
    out += R"(<p class="empty">No resolved facts.</p></section>)";
    return;
  }
  out += R"(<table class="facts" aria-labelledby="facts">
  <thead><tr><th scope="col">Target · Kind</th><th scope="col">Resolved value</th><th scope="col">Defined at</th></tr></thead><tbody>
)";
  for (const PlanFact& fact : facts) {
    out += "<tr><td>";
    append_owner(out, fact);
    out += "</td><td>";
    append_fact_value(out, fact);
    out += "</td><td>";
    append_location(out, fact, lines);
    out += "</td></tr>\n";
  }
  out += "</tbody></table></section>\n";
}

void append_manifest(std::string& out, std::string_view text) {
  out += R"(<section class="panel manifest" aria-labelledby="manifest"><h2 id="manifest">build.ycxx</h2>
<div class="source-scroll" tabindex="0" role="region" aria-labelledby="manifest"><pre><code>)";
  for (std::size_t line = 1; !text.empty(); ++line) {
    const std::size_t newline = text.find('\n');
    std::format_to(std::back_inserter(out), R"(<span class="source-line" id="manifest-line-{}" data-number="{}">)", line, line);
    escaped(out, text.substr(0, newline));
    out += "</span>";
    if (newline == std::string_view::npos) break;
    out += '\n';
    text.remove_prefix(newline + 1);
  }
  out += "</code></pre></div></section>\n";
}

}  // namespace
}  // namespace ycxx::build

export namespace ycxx::build {

std::string render_inspection_html(const PlanInspection& inspection,
                                   const std::optional<std::vector<PlanChange>>& changes = std::nullopt) {
  std::string out = R"(<!doctype html>
<html lang="en">
<head><meta charset="utf-8"><meta name="viewport" content="width=device-width, initial-scale=1">
<title>Build plan · )";
  escaped(out, inspection.target);
  out += "</title>";
  out += styles;
  out += "</head>\n<body><main><h1>Build plan for <code>";
  escaped(out, inspection.target);
  out += R"(</code></h1><div class="layout"><div class="results">)";
  const std::string_view definition = inspection.definition;
  const auto lines = static_cast<std::size_t>(std::ranges::count(definition, '\n'))
                     + (!definition.empty() && definition.back() != '\n');
  if (changes) append_changes(out, *changes, lines);
  append_facts(out, inspection.facts, lines);
  out += "</div>";
  append_manifest(out, definition);
  out += "</div></main></body></html>\n";
  return out;
}

}  // namespace ycxx::build
