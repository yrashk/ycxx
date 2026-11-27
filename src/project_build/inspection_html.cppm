// Standalone, source-linked build inspection report (#1256).
export module ycxx.build.inspection_html;

import std;
import ycxx.build.inspection_model;
import ycxx.build.module_graph_diff;

export namespace ycxx::build {

struct ModuleGraphComparison {
  const ModuleGraphInspection& before;
  const ModuleGraphChanges& changes;
};

}  // namespace ycxx::build

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
  main { max-width: 92rem; margin: 0 auto; padding: 2rem; padding: clamp(1rem, 4vw, 3.5rem); }
  h1, h2, p, pre { margin: 0; }
  h1 { margin-bottom: 1.5rem; font-size: 2.2rem; font-size: clamp(1.75rem, 4vw, 2.6rem); line-height: 1.12;
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
  .catalog th:first-child, .dependency-comparison th:first-child { width: 40%; }
  .catalog th:last-child { width: 31%; }
  .dependency-comparison th:last-child { width: 30%; }
  .dependencies th:last-child { width: 26%; }
  .catalog code, .dependencies code, .dependency-comparison code { white-space: pre-wrap; }
  .dependencies .origin, .dependency-comparison .origin { display: block; }
  .dependency-sites { margin: .15rem 0 0; padding-inline-start: 1rem; }
  .dependency-notice { padding: .75rem 1.1rem; background: #f8f7f2; }
  .plan-target { padding: .75rem 1.1rem; overflow-wrap: anywhere; scroll-margin-block: .65rem; }
  .plan-target + .plan-target { border-top: 1px solid var(--border); }
  .plan-target:target { background: #e5f1f3; box-shadow: inset .18rem 0 var(--link); }
  .plan-target h3 { margin: 0 0 .3rem; font-size: 1rem; }
  .plan-target code { white-space: pre-wrap; }
  .plan-target > ul { margin: 0; padding: 0; list-style: none; }
  .plan-target li { padding: .3rem 0; }
  .plan-target li .origin { margin-inline-start: .65rem; }
  .plan-target .empty { padding: 0; }
  section#target-graph { margin-bottom: 1.25rem; --graph-edge: #547485; }
  .graph-explorer { display: grid; grid-template-columns: minmax(0, 1fr) minmax(16rem, 29%); }
  .graph-main { display: flex; flex-direction: column; min-width: 0; padding: .85rem 1.1rem 1.1rem; }
  .graph-toolbar, .graph-controls { display: flex; flex-wrap: wrap; align-items: center; gap: .45rem .7rem; }
  span#graph-count { color: var(--muted); font-size: .85rem; }
  button#graph-reset, .graph-controls button, .graph-node {
    font: inherit; color: inherit; background: #fff; border: 1px solid #bbcbd6; border-radius: .4rem;
  }
  button#graph-reset, .graph-controls button { padding: .28rem .6rem; cursor: pointer; font-size: .84rem; }
  .graph-controls button:disabled { opacity: .6; cursor: default; }
  button#graph-reset:focus-visible, .graph-controls button:focus-visible, .graph-node:focus-visible,
  div#graph-viewport:focus-visible { outline: 2px solid #188496; outline-offset: 2px; }
  div#graph-viewport { flex: 1; margin-top: .75rem; min-height: 13rem; max-height: 27rem; overflow: auto;
    border: 1px solid var(--border); border-radius: .55rem; background-color: #f8fafc;
    background-image: radial-gradient(#dce4ec 1px, transparent 1px); background-size: 16px 16px; }
  div#graph-stage { position: relative; overflow: hidden; }
  canvas#graph-edges { display: block; position: absolute; inset: 0; pointer-events: none; }
  div#graph-nodes { position: absolute; inset: 0; }
  .graph-node { position: absolute; padding: .35rem .55rem; font: 650 .88rem/1.25 var(--mono);
    white-space: pre-wrap; overflow-wrap: anywhere; overflow: hidden; cursor: pointer; unicode-bidi: isolate; }
  .graph-node code { font-size: 1em; }
  .graph-node[hidden] { display: none; }
  .graph-node[data-root="true"] { border-color: #658291; }
  .graph-node[aria-pressed="true"] { color: #104a58; background: #e5f1f3; border-color: var(--link); }
  .graph-sidebar { min-width: 0; padding: .85rem 1.1rem 1rem; border-left: 1px solid var(--border); }
  .graph-sidebar h3 { margin: 0 0 .45rem; font: 650 1rem/1.3 var(--mono); overflow-wrap: anywhere; }
  div#graph-details { max-height: 27rem; overflow: auto; overflow-wrap: anywhere; font-size: .9rem; }
  .graph-group { margin: .85rem 0 .2rem; }
  .graph-group > h4 { margin: 0 0 .2rem; color: var(--muted); font-size: .8rem; }
  .graph-group > ul { margin: 0; padding: 0; list-style: none; }
  .graph-relation, .graph-fact { padding: .35rem 0; }
  .graph-relation + .graph-relation, .graph-fact + .graph-fact { border-top: 1px solid #e8edf2; }
  .graph-relation code, .graph-fact code { white-space: pre-wrap; }
  .graph-relation .origin { margin-inline-start: .45rem; }
  .graph-fact > strong { margin-inline-end: .5rem; font-size: .79rem; font-weight: 650; }
  .owner, .fact-name, .location { display: block; }
  .owner, .origin, .fact-name { color: var(--muted); font-size: .86rem; }
  .fact-value, .fact-name, .origin { white-space: pre-wrap; overflow-wrap: anywhere; }
  .location { font: .78rem/1.55 var(--mono); }
  .changes { list-style: none; margin: 0; padding: 0; }
  .changes li { display: grid; grid-template-columns: minmax(0, 22%) minmax(0, 1fr);
    gap: 1rem; padding: .8rem 1.1rem; overflow-wrap: anywhere; }
  .changes li + li { border-top: 1px solid #e8edf2; }
  .changes .origin { display: block; margin-top: .18rem; }
  .action { display: inline-block; font-size: .79rem; font-weight: 750; }
  .added { color: #116749; }
  .removed { color: #a0333a; }
  .changed { color: #785515; }
  .before, .after { display: grid; grid-template-columns: 3.15rem minmax(0, 1fr); gap: .55rem; align-items: baseline; }
  .before > span, .after > span { color: var(--muted); font-size: .78rem; }
  .before code { color: #a0333a; }
  .after code { color: #116749; }
  .empty { padding: .9rem 1.1rem; color: var(--muted); }
  .module-summary, .module-notice { padding: .75rem 1.1rem; overflow-wrap: anywhere; }
  .module-summary { display: flex; flex-wrap: wrap; justify-content: space-between; gap: .25rem 1rem; }
  .module-status, .module-meta { color: var(--muted); font-size: .83rem; }
  .module-notice { background: #f8f7f2; border-top: 1px solid var(--border); }
  .module-notice h3 { font-size: .92rem; margin: 0 0 .25rem; }
  .module-notice ul { margin: .25rem 0 0; padding-left: 1.2rem; }
  .module-unit { border-top: 1px solid var(--border); scroll-margin-block: .65rem; }
  .module-unit summary { padding: .65rem 1.1rem; cursor: pointer; overflow-wrap: anywhere; }
  .module-unit summary .module-meta { margin-left: .45rem; }
  .module-unit code, .module-notice code { white-space: pre-wrap; }
  .module-directives { margin: 0 1.1rem .7rem 2.15rem; padding: 0; list-style: none; }
  .module-directives > li { padding: .4rem .55rem; border-left: 2px solid #dbe2ea;
    overflow-wrap: anywhere; scroll-margin-block: .65rem; }
  .module-unit:target > summary, .module-directives > li:target { background: #e5f1f3; }
  .module-directive { display: flex; flex-wrap: wrap; justify-content: space-between; gap: .1rem .7rem; }
  .module-site { display: block; color: var(--muted); }
  .module-candidates { margin: .2rem 0 0; padding-left: 1.15rem; }
  .module-candidates li { padding: .06rem 0; }
  .module-change-group { padding: .65rem 1.1rem; border-top: 1px solid var(--border); overflow-wrap: anywhere; }
  .module-change-group h3 { margin: 0 0 .3rem; font-size: .95rem; }
  .module-change-group > ul { margin: 0; padding: 0; list-style: none; }
  .module-change { padding: .65rem 0; }
  .module-change + .module-change { border-top: 1px solid #e8edf2; }
  .module-change-heading { display: flex; flex-wrap: wrap; align-items: baseline; gap: .2rem .65rem; margin-bottom: .32rem; }
  .module-sides { display: grid; grid-template-columns: repeat(2, minmax(0, 1fr)); gap: .6rem; }
  .module-side { display: block; min-width: 0; padding: .5rem .65rem; border: 1px solid #e8edf2; border-radius: .4rem; }
  .module-side.before { background: #f9fafc; }
  .module-side.after { background: #f2f8f8; }
  .module-side > .side-label, .module-side > .module-status, .module-detail-label { display: block; }
  .module-side code { color: inherit; white-space: pre-wrap; }
  .module-side-items { margin: .2rem 0 0; padding-left: 1.15rem; }
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
    .graph-explorer { grid-template-columns: minmax(0, 1fr); }
    .graph-sidebar { border-left: 0; border-top: 1px solid var(--border); }
  }
  @media (max-width: 31rem) {
    .facts th, .facts td, .changes li { padding-inline: .6rem; }
    .facts th:first-child { width: 24%; }
    .module-sides { grid-template-columns: minmax(0, 1fr); }
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

void append_manifest_ref(std::string& out, std::string_view path, std::size_t line, std::size_t column, bool link) {
  auto next = std::back_inserter(out);
  if (link) std::format_to(next, R"(<a href="#manifest-line-{}">)", line);
  escaped(out, path);
  std::format_to(next, ":{}", line);
  if (column != 0) std::format_to(next, ":{}", column);
  if (link) out += "</a>";
}

void append_location(std::string& out, const PlanFact& fact, std::size_t lines) {
  if (!fact.origin.empty()) {
    out += R"(<span class="origin">)";
    escaped(out, fact.origin);
    out += "</span>";
  }
  if (fact.line == 0) return;
  out += R"(<span class="location">)";
  append_manifest_ref(out, "build.ycxx", fact.line, fact.column, fact.line <= lines);
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

void append_action(std::string& out, std::string_view change) {
  if (change == "added") out += R"(<span class="action added">Added</span>)";
  else if (change == "removed") out += R"(<span class="action removed">Removed</span>)";
  else out += R"(<span class="action changed">Changed</span>)";
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
    append_action(out, change.change);
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
  out += R"(<section class="panel" aria-labelledby="facts"><h2 id="facts">Plan facts</h2>)";
  if (facts.empty()) {
    out += R"(<p class="empty">No plan facts.</p></section>)";
    return;
  }
  out += R"(<table class="facts" aria-labelledby="facts">
  <thead><tr><th scope="col">Target · Kind</th><th scope="col">Value</th><th scope="col">Defined at</th></tr></thead><tbody>
)";
  constexpr std::string_view graph_kinds[] = {
    "rule", "source", "include", "output", "host-target", "native-target", "link-input",
  };
  for (const PlanFact& fact : facts) {
    out += "<tr";
    if (std::ranges::contains(graph_kinds, std::string_view(fact.kind))) {
      out += R"HTML( data-graph-target=")HTML";
      escaped(out, fact.target);
      out += R"HTML(" data-graph-kind=")HTML";
      escaped(out, fact.kind);
      out += '"';
    }
    out += "><td>";
    append_owner(out, fact);
    out += "</td><td>";
    append_fact_value(out, fact);
    out += "</td><td>";
    append_location(out, fact, lines);
    out += "</td></tr>\n";
  }
  out += "</tbody></table></section>\n";
}

void append_dependency_detail(std::string& out, std::string_view label, std::optional<std::string_view> value) {
  if (!value) return;
  out += R"(<span class="origin">)";
  out += label;
  out += " <code>";
  escaped(out, *value);
  out += "</code></span>";
}

void append_dependency_location(std::string& out, const ManifestLocation& location, std::size_t lines) {
  out += R"(<span class="location">)";
  const auto& [path, line, column] = location;
  append_manifest_ref(out, path, line, column, path == "build.ycxx" && line > 0 && line <= lines);
  out += "</span>";
}

template <class Edge>
void append_dependency_sites(std::string& out, const std::vector<Edge>& edges, std::size_t lines) {
  std::format_to(std::back_inserter(out), R"(<span class="origin">{} {}</span>)",
                 edges.size(), edges.size() == 1 ? "occurrence" : "occurrences");
  if (edges.empty()) return;
  out += R"(<ul class="dependency-sites">)";
  for (const auto& edge : edges) {
    out += "<li>";
    append_dependency_location(out, edge.location, lines);
    out += "</li>";
  }
  out += "</ul>";
}

template <class Edge>
void append_dependency_identity(std::string& out, const Edge& edge) {
  out += "<code>";
  escaped(out, edge.consumer);
  out += "</code> → <code>";
  escaped(out, edge.producer);
  out += R"(</code><code class="origin">)";
  escaped(out, edge.usage);
  out += "</code>";
}

void append_dependency_changes(std::string& out, std::span<const DependencyOccurrenceChange> changes, std::size_t lines) {
  out += R"(<section class="panel" aria-labelledby="dependency-changes"><h2 id="dependency-changes">Dependency changes</h2>)";
  if (changes.empty()) {
    out += R"(<p class="empty">No dependency changes.</p></section>)";
    return;
  }
  out += R"(<table class="facts dependency-comparison" aria-labelledby="dependency-changes">
  <thead><tr><th scope="col">Dependency</th><th scope="col">Before</th><th scope="col">After</th></tr></thead><tbody>
)";
  for (const auto& change : changes) {
    const auto& edge = change.after.empty() ? change.before.front() : change.after.front();
    out += "<tr><td>";
    append_dependency_identity(out, edge);
    append_dependency_detail(out, "Binding", edge.binding);
    append_dependency_detail(out, "Output", edge.output);
    out += "</td><td>";
    append_dependency_sites(out, change.before, 0);
    out += "</td><td>";
    append_dependency_sites(out, change.after, lines);
    out += "</td></tr>\n";
  }
  out += "</tbody></table></section>\n";
}

void append_value_binding_changes(std::string& out, std::span<const ValueBindingOccurrenceChange> changes,
                                  std::size_t lines) {
  out += R"(<section class="panel" aria-labelledby="value-binding-changes"><h2 id="value-binding-changes">Value binding changes</h2>)";
  if (changes.empty()) {
    out += R"(<p class="empty">No value binding changes.</p></section>)";
    return;
  }
  out += R"(<table class="facts dependency-comparison" aria-labelledby="value-binding-changes">
  <thead><tr><th scope="col">Value binding</th><th scope="col">Before</th><th scope="col">After</th></tr></thead><tbody>
)";
  for (const auto& change : changes) {
    const auto& edge = change.after.empty() ? change.before.front() : change.after.front();
    out += "<tr><td>";
    append_dependency_identity(out, edge);
    append_dependency_detail(out, "Binding", edge.binding);
    append_dependency_detail(out, "Export", edge.export_name);
    out += "</td><td>";
    append_dependency_sites(out, change.before, 0);
    out += "</td><td>";
    append_dependency_sites(out, change.after, lines);
    out += "</td></tr>\n";
  }
  out += "</tbody></table></section>\n";
}

void append_dependencies(std::string& out, const DependencyGraphInspection& graph, std::size_t lines) {
  out += R"(<section class="panel" aria-labelledby="dependencies"><h2 id="dependencies">Dependencies</h2>)";
  if (!graph.complete)
    out += R"(<p class="dependency-notice">Incomplete plan. Showing resolved dependencies so far.</p>)";
  if (graph.edges.empty()) {
    out += R"(<p class="empty">)";
    out += graph.complete ? "No dependencies." : "No resolved dependencies yet.";
    out += "</p></section>\n";
    return;
  }
  out += R"(<table class="facts dependencies" aria-labelledby="dependencies">
  <thead><tr><th scope="col">Consumer</th><th scope="col">Usage</th><th scope="col">Producer</th><th scope="col">Declared at</th></tr></thead><tbody>
)";
  for (const auto& edge : graph.edges) {
    out += "<tr><td><code>";
    escaped(out, edge.consumer);
    out += "</code></td><td><code>";
    escaped(out, edge.usage);
    out += "</code>";
    append_dependency_detail(out, "Binding", edge.binding);
    out += "</td><td><code>";
    escaped(out, edge.producer);
    out += "</code>";
    append_dependency_detail(out, "Output", edge.output);
    out += "</td><td>";
    append_dependency_location(out, edge.location, lines);
    out += "</td></tr>\n";
  }
  out += "</tbody></table></section>\n";
}

void append_value_bindings(std::string& out, std::span<const PlanValueDependencyEdge> edges,
                           bool complete, std::size_t lines) {
  out += R"(<section class="panel" aria-labelledby="value-bindings"><h2 id="value-bindings">Value bindings</h2>)";
  if (!complete)
    out += R"(<p class="dependency-notice">Incomplete plan. Showing resolved value bindings so far.</p>)";
  if (edges.empty()) {
    out += R"(<p class="empty">)";
    out += complete ? "No value bindings." : "No resolved value bindings yet.";
    out += "</p></section>\n";
    return;
  }
  out += R"(<table class="facts dependencies" aria-labelledby="value-bindings">
  <thead><tr><th scope="col">Consumer</th><th scope="col">Usage</th><th scope="col">Producer</th><th scope="col">Declared at</th></tr></thead><tbody>
)";
  for (const auto& edge : edges) {
    out += "<tr><td><code>";
    escaped(out, edge.consumer);
    out += "</code></td><td><code>";
    escaped(out, edge.usage);
    out += "</code>";
    append_dependency_detail(out, "Binding", edge.binding);
    out += "</td><td><code>";
    escaped(out, edge.producer);
    out += "</code>";
    append_dependency_detail(out, "Export", edge.export_name);
    out += "</td><td>";
    append_dependency_location(out, edge.location, lines);
    out += "</td></tr>\n";
  }
  out += "</tbody></table></section>\n";
}

void append_target_dependencies(std::string& out, std::string_view selected, const DependencyGraphInspection& graph,
                                std::size_t lines) {
  struct TargetInputs {
    std::size_t id = 0;
    std::vector<const PlanDependencyEdge*> artifacts;
    std::vector<const PlanValueDependencyEdge*> values;
  };
  std::map<std::string_view, TargetInputs> targets;
  const auto selected_target = targets.try_emplace(selected).first;
  for (const auto& edge : graph.edges) {
    targets[edge.consumer].artifacts.push_back(&edge);
    targets.try_emplace(edge.producer);
  }
  if (graph.value_edges) {
    for (const auto& edge : *graph.value_edges) {
      targets[edge.consumer].values.push_back(&edge);
      targets.try_emplace(edge.producer);
    }
  }
  std::size_t id = 0;
  for (auto& target : targets | std::views::values) target.id = id++;

  out += R"(<section class="panel" aria-labelledby="target-dependencies"><h2 id="target-dependencies">Target dependencies</h2>)";
  if (!graph.complete)
    out += R"(<p class="dependency-notice">Incomplete plan. Showing resolved target dependencies so far.</p>)";
  const auto append_input = [&](const auto& edge, std::string_view label, std::optional<std::string_view> detail) {
    std::format_to(std::back_inserter(out), R"(<li><a href="#plan-target-{}"><code>)", targets.at(edge.producer).id);
    escaped(out, edge.producer);
    out += R"(</code></a><code class="origin">)";
    escaped(out, edge.usage);
    out += "</code>";
    append_dependency_detail(out, "Binding", edge.binding);
    append_dependency_detail(out, label, detail);
    append_dependency_location(out, edge.location, lines);
    out += "</li>\n";
  };
  const auto append_target = [&](std::string_view name, const TargetInputs& target) {
    std::format_to(std::back_inserter(out), R"(<article class="plan-target" id="plan-target-{}"><h3><code>)", target.id);
    escaped(out, name);
    out += "</code></h3>";
    if (target.artifacts.empty() && target.values.empty()) {
      out += R"(<p class="empty">)";
      out += graph.complete ? "No resolved inputs." : "No resolved inputs yet.";
      out += "</p>";
    } else {
      out += "<ul>";
      for (const auto* edge : target.artifacts) append_input(*edge, "Output", edge->output);
      for (const auto* edge : target.values) append_input(*edge, "Export", edge->export_name);
      out += "</ul>";
    }
    out += "</article>\n";
  };
  append_target(selected_target->first, selected_target->second);
  for (const auto& [name, target] : targets)
    if (name != selected) append_target(name, target);
  out += "</section>\n";
}

constexpr std::string_view graph_script = R"YCXX_GRAPH(<script>
(() => {
  const byId = (id) => document.getElementById(id);
  const panel = byId("target-graph");
  const cards = document.querySelector('section[aria-labelledby="target-dependencies"]');
  if (!panel || !cards) return;

  const viewport = byId("graph-viewport");
  const stage = byId("graph-stage");
  const canvas = byId("graph-edges");
  const nodes = byId("graph-nodes");
  const heading = byId("graph-target");
  const count = byId("graph-count");
  const details = byId("graph-details");
  const expandInputs = byId("graph-expand-inputs");
  const expandConsumers = byId("graph-expand-consumers");
  const reset = byId("graph-reset");
  if (![viewport, stage, canvas, nodes, heading, count, details, expandInputs, expandConsumers, reset].every(Boolean)) return;

  const targets = new Map();
  for (const card of cards.querySelectorAll(":scope > article.plan-target")) {
    const title = card.querySelector(":scope > h3 > code");
    if (title && card.id) targets.set(card.id, {card, name: title.textContent, inputs: [], consumers: []});
  }
  const root = targets.values().next().value;
  if (!root) return;
  for (const target of targets.values()) {
    for (const row of target.card.querySelectorAll(":scope > ul > li")) {
      const link = row.querySelector("a");
      const fragment = link?.getAttribute("href");
      const producer = fragment?.startsWith("#") && targets.get(fragment.slice(1));
      if (!producer) continue;
      const edge = {consumer: target, producer, row};
      target.inputs.push(edge);
      producer.consumers.push(edge);
    }
  }

  const factLabels = new Map([
    ["rule", "Rule"], ["host-target", "Host target"], ["native-target", "Native target"],
    ["source", "Source"], ["include", "Include path"], ["output", "Output"], ["link-input", "Link input"],
  ]);
  const facts = new Map();
  for (const row of document.querySelectorAll("tr[data-graph-target][data-graph-kind]")) {
    const {graphTarget: name, graphKind: kind} = row.dataset;
    if (!factLabels.has(kind) || row.cells.length < 3) continue;
    if (!facts.has(name)) facts.set(name, []);
    facts.get(name).push({row, label: factLabels.get(kind)});
  }

  const inputsOpen = new Set([root]);
  const consumersOpen = new Set();
  const buttons = new Map();
  const nodeWidth = 174;
  const nodeHeight = 54;
  const columnStep = 242;
  const rowStep = 74;
  const margin = 26;
  let selected = root;
  let positions = new Map();
  let links = [];
  let paintPending = false;

  function nodeNeighborhood() {
    // A target gets its first breadth-first position, even when another branch shares it.
    const depths = new Map([[root, 0]]);
    const queue = [root];
    for (const target of queue) {
      const depth = depths.get(target);
      const visit = (neighbor, nextDepth) => {
        if (depths.has(neighbor)) return;
        depths.set(neighbor, nextDepth);
        queue.push(neighbor);
      };
      if (inputsOpen.has(target))
        for (const edge of target.inputs) visit(edge.producer, depth + 1);
      if (consumersOpen.has(target))
        for (const edge of target.consumers) visit(edge.consumer, depth - 1);
    }
    return depths;
  }

  function list(title, items) {
    if (!items.length) return;
    const group = document.createElement("section");
    group.className = "graph-group";
    const label = document.createElement("h4");
    label.textContent = title;
    const rows = document.createElement("ul");
    for (const item of items) rows.append(item);
    group.append(label, rows);
    details.append(group);
  }

  function relations(edges, incoming) {
    return edges.map((edge) => {
      const row = edge.row.cloneNode(true);
      row.classList.add("graph-relation");
      if (incoming) {
        const link = row.querySelector("a");
        link.setAttribute("href", "#" + edge.consumer.card.id);
        link.querySelector("code").textContent = edge.consumer.name;
      }
      return row;
    });
  }

  function showSelection() {
    heading.textContent = selected.name;
    details.replaceChildren();
    list("Dependencies", relations(selected.inputs, false));
    list("Dependents", relations(selected.consumers, true));
    list("Plan facts", (facts.get(selected.name) ?? []).map(({row, label}) => {
      const item = document.createElement("li");
      item.className = "graph-fact";
      const name = document.createElement("strong");
      name.textContent = label;
      item.append(name, " ");
      for (const cell of [row.cells[1], row.cells[2]]) {
        for (const child of cell.childNodes) item.append(child.cloneNode(true));
        item.append(" ");
      }
      return item;
    }));
  }

  function updateControls() {
    for (const [button, open, label, edges] of [
      [expandInputs, inputsOpen, "dependencies", selected.inputs],
      [expandConsumers, consumersOpen, "dependents", selected.consumers],
    ]) {
      const expanded = edges.length !== 0 && open.has(selected);
      button.textContent = (expanded ? "Collapse " : "Expand ") + label;
      button.disabled = edges.length === 0;
      button.setAttribute("aria-expanded", String(expanded));
    }
  }

  function select(target) {
    if (selected === target) return;
    buttons.get(selected)?.setAttribute("aria-pressed", "false");
    selected = target;
    buttons.get(selected).setAttribute("aria-pressed", "true");
    updateControls();
    showSelection();
  }

  function nodeButton(target) {
    if (buttons.has(target)) return buttons.get(target);
    const button = document.createElement("button");
    button.className = "graph-node";
    button.type = "button";
    button.title = target.name;
    if (target === root) button.dataset.root = "true";
    const name = document.createElement("code");
    name.textContent = target.name;
    button.append(name);
    button.addEventListener("click", () => select(target));
    buttons.set(target, button);
    nodes.append(button);
    return button;
  }

  function curve(from, to) {
    const startY = from.y + nodeHeight / 2;
    const endY = to.y + nodeHeight / 2;
    if (from.x === to.x) {
      const side = from.x + nodeWidth;
      if (from === to)
        return [side, startY - 11, side + 42, startY - 21, side + 42, startY + 21, side, startY + 11];
      return [side, startY, side + 38, startY, side + 38, endY, side, endY];
    }
    const facingRight = from.x < to.x;
    const startX = from.x + (facingRight ? nodeWidth : 0);
    const endX = to.x + (facingRight ? 0 : nodeWidth);
    const middle = (startX + endX) / 2;
    return [startX, startY, middle, startY, middle, endY, endX, endY];
  }

  function paint() {
    paintPending = false;
    const context = canvas.getContext("2d");
    const width = viewport.clientWidth;
    const height = viewport.clientHeight;
    if (!context || !width || !height) return;
    const left = viewport.scrollLeft;
    const top = viewport.scrollTop;
    const scale = Math.min(window.devicePixelRatio || 1, 2);
    const pixelsWide = Math.ceil(width * scale);
    const pixelsHigh = Math.ceil(height * scale);
    if (canvas.width !== pixelsWide) canvas.width = pixelsWide;
    if (canvas.height !== pixelsHigh) canvas.height = pixelsHigh;
    Object.assign(canvas.style, {left: left + "px", top: top + "px", width: width + "px", height: height + "px"});
    context.setTransform(1, 0, 0, 1, 0, 0);
    context.clearRect(0, 0, pixelsWide, pixelsHigh);
    context.setTransform(scale, 0, 0, scale, -left * scale, -top * scale);
    context.strokeStyle = getComputedStyle(panel).getPropertyValue("--graph-edge").trim() || "#596e83";
    context.lineWidth = 1.5;
    for (const [sx, sy, c1x, c1y, c2x, c2y, ex, ey] of links) {
      if (Math.max(sx, c1x, c2x, ex) < left || Math.min(sx, c1x, c2x, ex) > left + width ||
          Math.max(sy, c1y, c2y, ey) < top || Math.min(sy, c1y, c2y, ey) > top + height) continue;
      const angle = Math.atan2(ey - c2y, ex - c2x);
      context.beginPath();
      context.moveTo(sx, sy);
      context.bezierCurveTo(c1x, c1y, c2x, c2y, ex, ey);
      context.moveTo(ex - 9 * Math.cos(angle - 0.5), ey - 9 * Math.sin(angle - 0.5));
      context.lineTo(ex, ey);
      context.lineTo(ex - 9 * Math.cos(angle + 0.5), ey - 9 * Math.sin(angle + 0.5));
      context.stroke();
    }
  }

  function schedulePaint() {
    if (paintPending) return;
    paintPending = true;
    requestAnimationFrame(paint);
  }

  function render() {
    const depths = nodeNeighborhood();
    const rows = new Map();
    let first = 0;
    let last = 0;
    for (const depth of depths.values()) {
      first = Math.min(first, depth);
      last = Math.max(last, depth);
    }

    const previous = positions.get(selected);
    positions = new Map();
    let height = 0;
    for (const [target, depth] of depths) {
      const row = rows.get(depth) ?? 0;
      const position = {x: margin + (depth - first) * columnStep, y: margin + row * rowStep};
      rows.set(depth, row + 1);
      positions.set(target, position);
      height = Math.max(height, position.y + nodeHeight + margin);
      const button = nodeButton(target);
      Object.assign(button.style, {
        left: position.x + "px", top: position.y + "px", width: nodeWidth + "px", height: nodeHeight + "px",
      });
      button.hidden = false;
      button.setAttribute("aria-pressed", String(target === selected));
    }
    for (const [target, button] of buttons) button.hidden = !positions.has(target);

    links = [];
    for (const [target, from] of positions) {
      // Draw one connection per pair. The details retain every authored occurrence.
      for (const producer of new Set(target.inputs.map((edge) => edge.producer))) {
        const to = positions.get(producer);
        if (to) links.push(curve(from, to));
      }
    }
    stage.style.width = (margin * 2 + (last - first) * columnStep + nodeWidth + 42) + "px";
    stage.style.height = height + "px";
    if (previous) {
      const next = positions.get(selected);
      viewport.scrollLeft += next.x - previous.x;
      viewport.scrollTop += next.y - previous.y;
    }
    count.textContent = positions.size + " of " + targets.size + " targets shown";
    updateControls();
    schedulePaint();
  }

  function toggle(open) {
    if (!open.delete(selected)) open.add(selected);
    render();
  }

  expandInputs.addEventListener("click", () => toggle(inputsOpen));
  expandConsumers.addEventListener("click", () => toggle(consumersOpen));
  reset.addEventListener("click", () => {
    inputsOpen.clear();
    inputsOpen.add(root);
    consumersOpen.clear();
    select(root);
    render();
    viewport.scrollLeft = 0;
    viewport.scrollTop = 0;
  });
  viewport.addEventListener("scroll", schedulePaint, {passive: true});
  if (typeof ResizeObserver === "function") new ResizeObserver(schedulePaint).observe(viewport);
  else window.addEventListener("resize", schedulePaint);
  panel.hidden = false;
  showSelection();
  render();
})();
</script>
)YCXX_GRAPH";

void append_graph_explorer(std::string& out, bool complete) {
  out += R"HTML(<section class="panel" id="target-graph" hidden aria-labelledby="target-graph-heading">
<h2 id="target-graph-heading">Graph</h2>
)HTML";
  if (!complete) out += R"(<p class="dependency-notice">Incomplete plan. Showing resolved connections.</p>)";
  out += R"HTML(<div class="graph-explorer">
<div class="graph-main">
  <div class="graph-toolbar"><button id="graph-reset" type="button">Reset graph</button><span id="graph-count" role="status"></span></div>
  <div id="graph-viewport" role="region" tabindex="0" aria-label="Build dependency graph">
    <div id="graph-stage"><canvas id="graph-edges" aria-hidden="true"></canvas><div id="graph-nodes"></div></div>
  </div>
</div>
<div class="graph-sidebar">
  <h3 id="graph-target"></h3>
  <div class="graph-controls">
    <button id="graph-expand-inputs" type="button" aria-controls="graph-nodes">Show dependencies</button>
    <button id="graph-expand-consumers" type="button" aria-controls="graph-nodes">Show dependents</button>
  </div>
  <div id="graph-details"></div>
</div>
</div></section>
)HTML";
}

std::string_view module_label(std::string_view value) {
  using Label = std::pair<std::string_view, std::string_view>;
  constexpr Label labels[] = {
      {"interface_unit", "interface unit"}, {"implementation_unit", "implementation unit"},
      {"exported_import", "export import"}, {"generated_inputs_unavailable", "generated inputs unavailable"},
      {"preprocessing_incomplete", "preprocessing incomplete"}, {"unresolved_partition", "unresolved partition"},
      {"external_or_unresolved", "external or unresolved"},
  };
  const auto found = std::ranges::find(labels, value, &Label::first);
  return found == std::ranges::end(labels) ? value : found->second;
}

void append_module_site(std::string& out, const ModuleSourceLocation& location) {
  escaped(out, location.path);
  std::format_to(std::back_inserter(out), ":{}:{}", location.line, location.column);
}

using ModuleDeclarations = std::map<std::pair<std::size_t, std::size_t>, const ModuleDeclarationInspection*>;
using ModuleSources = std::map<std::size_t, std::string_view>;

struct ModuleSites {
  ModuleDeclarations declarations;
  ModuleSources sources;
};

ModuleSites index_module_sites(const ModuleGraphInspection& graph) {
  ModuleSites sites;
  for (const auto& unit : graph.units) {
    sites.sources.emplace(unit.id, unit.source);
    for (const auto& declaration : unit.declarations)
      sites.declarations.emplace(std::pair{unit.id, declaration.directive_index}, &declaration);
  }
  return sites;
}

void append_module_site_link(std::string& out, const ModuleSourceLocation& location, std::string_view kind,
                             std::size_t unit, std::size_t index, bool link) {
  if (link) std::format_to(std::back_inserter(out), R"(<a href="#module-{}-{}-{}">)", kind, unit, index);
  out += "<code>";
  append_module_site(out, location);
  out += "</code>";
  if (link) out += "</a>";
}

void append_module_source_link(std::string& out, std::string_view source, std::size_t unit, bool link) {
  if (link) std::format_to(std::back_inserter(out), R"(<a href="#module-unit-{}">)", unit);
  out += "<code>";
  escaped(out, source);
  std::format_to(std::back_inserter(out), R"(</code> <span class="module-meta">· Unit {}</span>)", unit);
  if (link) out += "</a>";
}

void append_module_candidates(std::string& out, const std::vector<ModuleCandidateInspection>& candidates,
                              const ModuleDeclarations& declarations, bool link = true) {
  if (candidates.empty()) return;
  out += R"(<ul class="module-candidates">)";
  for (const auto& candidate : candidates) {
    const auto [unit, index] = candidate;
    out += "<li>";
    append_module_site_link(out, declarations.at({unit, index})->location, "declaration", unit, index, link);
    std::format_to(std::back_inserter(out), R"( <span class="module-meta">· Unit {}</span></li>)", unit);
  }
  out += "</ul>";
}

void append_module_directive(std::string& out, std::string_view kind, std::string_view name,
                             const ModuleSourceLocation& location, std::string_view status = {}) {
  out += R"(<div class="module-directive"><code>)";
  escaped(out, module_label(kind));
  out += ' ';
  escaped(out, name);
  out += "</code>";
  if (!status.empty()) {
    out += R"(<span class="module-status">)";
    escaped(out, module_label(status));
    out += "</span>";
  }
  out += R"(</div><code class="module-site">)";
  append_module_site(out, location);
  out += "</code>";
}

void append_module_unit(std::string& out, const ModuleUnitInspection& unit,
                        std::span<const ModuleImportInspection* const> imports, const ModuleDeclarations& declarations) {
  const bool has_directives = !unit.declarations.empty() || !imports.empty();
  std::format_to(std::back_inserter(out), R"(<details class="module-unit" id="module-unit-{}"{}><summary><code>)",
                 unit.id, has_directives || unit.scan != "complete" ? " open" : "");
  escaped(out, unit.source);
  std::format_to(std::back_inserter(out), R"(</code><span class="module-meta">Unit {})", unit.id);
  if (unit.scan != "complete") {
    out += " · ";
    escaped(out, module_label(unit.scan));
  }
  out += "</span></summary>";
  if (has_directives) {
    out += R"(<ul class="module-directives" role="list">)";
    for (const auto& declaration : unit.declarations) {
      std::format_to(std::back_inserter(out), R"(<li id="module-declaration-{}-{}">)", unit.id, declaration.directive_index);
      append_module_directive(out, declaration.kind, declaration.name, declaration.location);
      out += "</li>";
    }
    for (const auto* import : imports) {
      std::format_to(std::back_inserter(out), R"(<li id="module-import-{}-{}">)", unit.id, import->directive_index);
      append_module_directive(out, import->kind, import->name, import->location, import->status);
      append_module_candidates(out, import->candidates, declarations);
      out += "</li>";
    }
    out += "</ul>";
  } else if (unit.scan == "complete") {
    out += R"(<p class="empty">No named-module directives.</p>)";
  }
  out += "</details>\n";
}

void append_module_graph(std::string& out, const ModuleGraphInspection& graph, const ModuleSites& sites) {
  out += R"(<section class="panel" aria-labelledby="module-graph"><h2 id="module-graph">Module graph</h2>)";
  out += R"(<p class="module-summary"><code>)";
  escaped(out, graph.native_target);
  out += R"(</code><span class="module-status">)";
  out += graph.complete ? "Complete" : "Incomplete";
  out += "</span></p>";
  if (graph.units.empty()) {
    out += R"(<p class="empty">No C++ translation units.</p></section>)";
    return;
  }

  std::map<std::size_t, std::vector<const ModuleImportInspection*>> imports;
  for (const auto& import : graph.imports) imports[import.unit_id].push_back(&import);

  if (!graph.conflicts.empty()) {
    out += R"(<div class="module-notice"><h3>Provider conflicts</h3><ul>)";
    for (const auto& conflict : graph.conflicts) {
      out += "<li><code>";
      escaped(out, conflict.name);
      out += "</code>";
      append_module_candidates(out, conflict.candidates, sites.declarations);
      out += "</li>";
    }
    out += "</ul></div>";
  }
  if (!graph.cycles.empty()) {
    out += R"(<div class="module-notice"><h3>Cyclic components</h3><ul>)";
    for (const auto& cycle : graph.cycles) {
      std::string_view separator;
      out += "<li>";
      for (const auto unit : cycle) {
        out += separator;
        append_module_source_link(out, sites.sources.at(unit), unit, true);
        separator = ", ";
      }
      out += "</li>";
    }
    out += "</ul></div>";
  }
  for (const auto& unit : graph.units) {
    std::span<const ModuleImportInspection* const> edges;
    if (const auto found = imports.find(unit.id); found != imports.end()) edges = found->second;
    append_module_unit(out, unit, edges, sites.declarations);
  }
  out += "</section>\n";
}

struct ModuleSide {
  const ModuleSites& sites;
  bool current;
};

using ModuleSides = std::array<ModuleSide, 2>;

void append_changed_import(std::string& out, const ModuleImportInspection& edge, ModuleSide side) {
  out += R"(<span class="module-status">)";
  escaped(out, module_label(edge.kind));
  out += " · ";
  escaped(out, module_label(edge.status));
  out += "</span>";
  append_module_site_link(out, edge.location, "import", edge.unit_id, edge.directive_index, side.current);
  std::format_to(std::back_inserter(out), R"( <span class="module-meta">· Unit {}</span>)", edge.unit_id);
  if (!edge.candidates.empty()) out += R"(<span class="module-meta module-detail-label">Provider candidates</span>)";
  append_module_candidates(out, edge.candidates, side.sites.declarations, side.current);
}

void append_changed_unit(std::string& out, const ModuleUnitInspection& unit, ModuleSide side) {
  append_module_source_link(out, unit.source, unit.id, side.current);
  out += R"(<span class="module-status">)";
  escaped(out, module_label(unit.scan));
  out += "</span>";
  if (unit.configuration && !unit.configuration->include_dirs.empty()) {
    out += R"(<span class="module-meta module-detail-label">Include path order</span><ol class="module-side-items">)";
    for (const auto& directory : unit.configuration->include_dirs) {
      out += "<li><code>";
      escaped(out, directory);
      out += "</code></li>";
    }
    out += "</ol>";
  }
  if (unit.declarations.empty()) return;
  out += R"(<ul class="module-side-items">)";
  for (const auto& declaration : unit.declarations) {
    out += "<li><code>";
    escaped(out, module_label(declaration.kind));
    out += ' ';
    escaped(out, declaration.name);
    out += "</code><br>";
    append_module_site_link(out, declaration.location, "declaration", unit.id, declaration.directive_index, side.current);
    out += "</li>";
  }
  out += "</ul>";
}

void append_changed_cycle(std::string& out, const std::vector<std::size_t>& members, ModuleSide side) {
  out += R"(<ul class="module-side-items">)";
  for (const auto unit : members) {
    out += "<li>";
    append_module_source_link(out, side.sites.sources.at(unit), unit, side.current);
    out += "</li>";
  }
  out += "</ul>";
}

template <class Record, class Render>
void append_module_change(std::string& out, const ModuleChange<Record>& change, const ModuleSides& sides,
                          std::string_view subject, Render render) {
  out += R"(<li class="module-change"><div class="module-change-heading">)";
  append_action(out, change.change);
  if (!subject.empty()) {
    out += "<code>";
    escaped(out, subject);
    out += "</code>";
  }
  out += R"(</div><div class="module-sides">)";
  if (change.before) {
    out += R"(<div class="module-side before"><span class="side-label">Before</span>)";
    render(out, *change.before, sides[0]);
    out += "</div>";
  }
  if (change.after) {
    out += R"(<div class="module-side after"><span class="side-label">After</span>)";
    render(out, *change.after, sides[1]);
    out += "</div>";
  }
  out += "</div></li>\n";
}

template <class Record, class Subject, class Render>
void append_module_change_group(std::string& out, std::string_view title,
                                std::span<const ModuleChange<Record>> changes, const ModuleSides& sides,
                                Subject subject, Render render) {
  if (changes.empty()) return;
  out += R"(<section class="module-change-group"><h3>)";
  escaped(out, title);
  out += R"(</h3><ul role="list">)";
  for (const auto& change : changes)
    append_module_change(out, change, sides, subject(change.after ? *change.after : *change.before), render);
  out += "</ul></section>\n";
}

void append_module_comparison(std::string& out, const ModuleGraphComparison& comparison,
                              const ModuleGraphInspection& current, const ModuleSites& current_sites) {
  const auto& changes = comparison.changes;
  out += R"(<section class="panel" aria-labelledby="module-changes"><h2 id="module-changes">Module changes</h2>)";
  out += R"(<p class="module-summary"><span>Before <code>)";
  escaped(out, comparison.before.native_target);
  out += R"(</code></span><span>After <code>)";
  escaped(out, current.native_target);
  out += "</code></span></p>";
  if (changes.unavailable) {
    out += R"(<p class="module-notice"><strong>Module comparison unavailable.</strong> The saved snapshot lacks configuration identities for a source with multiple units.</p></section>)";
    return;
  }
  if (changes.matching)
    out += R"(<p class="module-notice">Older graph units were matched by source path.</p>)";
  if (changes.imports.empty() && changes.units.empty() && changes.conflicts.empty() && changes.cycles.empty() &&
      !changes.completeness) {
    out += R"(<p class="empty">No semantic module changes.</p></section>)";
    return;
  }

  const auto saved_sites = index_module_sites(comparison.before);
  const ModuleSides sides{{{saved_sites, false}, {current_sites, true}}};
  const auto unnamed = [](const auto&) { return std::string_view{}; };
  const auto name = [](const auto& change) -> std::string_view { return change.name; };
  append_module_change_group(out, "Import changes", std::span{changes.imports}, sides, name, append_changed_import);
  append_module_change_group(out, "Translation unit changes", std::span{changes.units}, sides, unnamed, append_changed_unit);
  append_module_change_group(out, "Provider conflict changes", std::span{changes.conflicts}, sides, name,
      [](std::string& html, const ModuleConflictInspection& conflict, ModuleSide side) {
        append_module_candidates(html, conflict.candidates, side.sites.declarations, side.current);
      });
  append_module_change_group(out, "Cycle changes", std::span{changes.cycles}, sides, unnamed, append_changed_cycle);
  if (changes.completeness)
    append_module_change_group(out, "Scan completeness", std::span{&*changes.completeness, 1}, sides, unnamed,
        [](std::string& html, bool complete, ModuleSide) { html += complete ? "Complete" : "Incomplete"; });
  out += "</section>\n";
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

std::string begin_report(std::string_view title) {
  std::string out = R"(<!doctype html>
<html lang="en">
<head><meta charset="utf-8"><meta name="viewport" content="width=device-width, initial-scale=1">
<title>)";
  escaped(out, title);
  out += "</title>";
  out += styles;
  out += "</head>\n<body><main>";
  return out;
}

}  // namespace
}  // namespace ycxx::build

export namespace ycxx::build {

std::string render_inspection_html(const PlanInspection& inspection,
                                   const std::vector<PlanChange>* changes = nullptr,
                                   const std::optional<ModuleGraphComparison>& modules = std::nullopt,
                                   const std::optional<std::vector<DependencyOccurrenceChange>>& dependency_changes =
                                       std::nullopt,
                                   const std::optional<std::vector<ValueBindingOccurrenceChange>>& value_binding_changes =
                                       std::nullopt) {
  auto out = begin_report(std::format("Build plan · {}", inspection.target));
  out += "<h1>Build plan for <code>";
  escaped(out, inspection.target);
  out += "</code></h1>";
  if (inspection.dependency_graph) append_graph_explorer(out, inspection.dependency_graph->complete);
  out += R"(<div class="layout"><div class="results">)";
  const std::string_view definition = inspection.definition;
  const auto lines = static_cast<std::size_t>(std::ranges::count(definition, '\n'))
                     + (!definition.empty() && definition.back() != '\n');
  if (inspection.module_graph) {
    const auto sites = index_module_sites(*inspection.module_graph);
    if (modules) append_module_comparison(out, *modules, *inspection.module_graph, sites);
    if (changes) append_changes(out, *changes, lines);
    append_module_graph(out, *inspection.module_graph, sites);
  } else if (changes) {
    append_changes(out, *changes, lines);
  }
  if (dependency_changes) append_dependency_changes(out, *dependency_changes, lines);
  if (value_binding_changes) append_value_binding_changes(out, *value_binding_changes, lines);
  if (inspection.dependency_graph) {
    const auto& graph = *inspection.dependency_graph;
    append_target_dependencies(out, inspection.target, graph, lines);
    append_dependencies(out, graph, lines);
    if (graph.value_edges) append_value_bindings(out, *graph.value_edges, graph.complete, lines);
  }
  append_facts(out, inspection.facts, lines);
  out += "</div>";
  append_manifest(out, definition);
  out += "</div></main>";
  if (inspection.dependency_graph) out += graph_script;
  out += "</body></html>\n";
  return out;
}

std::string render_target_catalog_html(const TargetCatalog& catalog, std::string_view definition) {
  auto out = begin_report("Workspace targets");
  out += R"(<h1 id="workspace-targets">Workspace targets</h1><div class="layout"><section class="panel" aria-labelledby="workspace-targets">)";
  if (catalog.targets.empty()) {
    out += R"(<p class="empty">No target declarations.</p>)";
  } else {
    out += R"(<table class="facts catalog" aria-labelledby="workspace-targets">
<thead><tr><th scope="col">Target</th><th scope="col">Rule</th><th scope="col">Declared at</th></tr></thead><tbody>
)";
    for (const auto& entry : catalog.targets) {
      out += "<tr><td><code>";
      escaped(out, entry.name);
      out += "</code></td><td><code>";
      escaped(out, entry.kind);
      out += R"(</code></td><td><span class="location">)";
      const auto& [path, line, column] = entry.location;
      append_manifest_ref(out, path, line, column, true);
      out += "</span></td></tr>\n";
    }
    out += "</tbody></table>";
  }
  out += "</section>";
  append_manifest(out, definition);
  out += "</div></main></body></html>\n";
  return out;
}

}  // namespace ycxx::build
