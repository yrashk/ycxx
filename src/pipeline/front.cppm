// The front half of a translation unit's pipeline (#43, #63), as the engine query `TuFront`: the
// file's text as the engine's input, `pp` and the item splitter as engine queries, then the parser
// and sema (`item_sema`, `analyze_body`) through `parse::Program`, which plays the engine for them
// inside this one cell until contextual queries exist (#16). Its value is the diagnostics, located
// by byte offset, and the `TranslationUnit` the back half compiles; `tu_unit(TU)` projects the
// latter for the back half, so a front end that re-runs with the same definitions changes nothing
// behind it.
export module ycxx.pipeline:front;

import std;
import ycxx.ast;
import ycxx.engine;
import ycxx.pp;
import ycxx.items;
import ycxx.sema;
import ycxx.parse;
import :back;
import :lines;

export namespace ycxx::pipeline {

// A diagnostic ready to render: a byte offset in the file, its severity and its message.
struct Message {
  std::uint32_t offset = 0;
  bool error = true;
  std::string text;
  // `file:line:col` when the diagnostic is in another file than the main one (an included
  // header); the offset is then meaningless. Until #34's renderer.
  std::string where;
  // The region token it is at or before: messages are in this order, across files.
  std::uint32_t token = 0;
};

// A function body of the unit, for an editor's position requests (#170): its item, the
// declaration part's length (the body tree's item-relative offset) and its tree.
struct BodyAt {
  ast::ItemId item{};
  std::uint32_t offset = 0;
  ast::NodeId tree = ast::NodeId::none;
};

struct FrontResult {
  std::vector<Message> messages;  // in order of position in the unit
  TranslationUnit unit;           // meaningful when there are no errors
  Shapes shapes;                  // its classes and enumerations, for `layout` (#115)
  // For an editor (#170), with errors or not: each entity's first declaration in the unit (its
  // declarator, else its item's start), in item order, and the analysed function bodies.
  std::vector<std::pair<EntityId, ast::Loc>> declared;
  std::vector<BodyAt> bodies;
  bool has_errors() const {
    return std::ranges::any_of(messages, [](const Message& m) { return m.error; });
  }
};

// One translation unit from its source file (`TuSource::file`), analysed.
struct TuFront {
  using Key = TuId;
  using Value = FrontResult;
  static engine::query_task<Value> run(engine::Ctx& cx, TuId tu);
};

// Go-to-definition (#170, ast.md §8.5): the entity named at a byte offset of a file of a unit (the
// main file or a header it includes), and the place it is first declared in the unit, as a file
// (absolute) and a byte offset. Names in function bodies so far: a name that resolved to an entity
// (`DeclRef`), a member (`Member`) or a called function (`Call`).
struct DefinitionKey {
  TuId tu;
  pp::PathId file;
  std::uint32_t offset = 0;
  bool operator==(const DefinitionKey&) const = default;
};
struct FileOffset {
  std::string path;
  std::uint32_t offset = 0;
};
struct DefinitionAt {
  using Key = DefinitionKey;
  using Value = std::optional<FileOffset>;
  static engine::query_task<Value> run(engine::Ctx& cx, DefinitionKey k);
};

// String literal objects and their addresses, from the values interner (`VString`, `VLvalue`).
class ValueConstants final : public Constants {
 public:
  explicit ValueConstants(sema::World& w) : w_(&w) {}
  std::optional<std::vector<std::uint8_t>> string_object(ConstId c) const override {
    sema::ValueRec r = w_->values.rec(c);
    if (r.kind != sema::ValueKind::string) return std::nullopt;
    return std::vector<std::uint8_t>(r.units.begin(), r.units.end());
  }
  std::optional<Address> address(ConstId c) const override {
    sema::ValueRec r = w_->values.rec(c);
    if (r.kind != sema::ValueKind::address) return std::nullopt;
    return Address{r.base, r.offset};
  }
  ConstId address_of(ConstId object) override { return w_->values.address({}, object, 0); }

 private:
  sema::World* w_;
};

// The byte offset of an item location (ast.md §8.3, steps 1–3) in the region's main file.
std::uint32_t byte_offset(ast::Interners& in, const pp::Region& region, const items::Items& items, ast::Loc at);

}  // namespace ycxx::pipeline

namespace ycxx::pipeline {

std::uint32_t byte_offset(ast::Interners& in, const pp::Region& region, const items::Items& items, ast::Loc at) {
  for (const items::Item& it : items.items) {
    if (in.items.add<ast::ItemId>(it.id) != at.item) continue;
    std::uint32_t tok = std::min<std::uint32_t>(it.begin + at.offset, static_cast<std::uint32_t>(region.locs.size() - 1));
    return region.site(tok).offset;  // a macro's token: where it was expanded
  }
  return 0;
}

// The definitions and declarations of the global namespace, in item order.
TranslationUnit unit_of(parse::Program& program, sema::World& world, const items::Items& items, std::string name) {
  ast::Interners& ins = world.ins;
  TranslationUnit tu;
  tu.name = std::move(name);
  std::set<std::uint32_t> seen;
  // The type from the declaration, not the entity's record: a variable's type is not part of its
  // identity, so a record made before an edit may hold the old one (#115).
  auto declare = [&](const ast::DeclSema& d, std::optional<ast::Loc> at) {
    EntityId e = d.entity;
    if (e == EntityId::none || !seen.insert(std::to_underlying(e)).second) return;
    const sema::EntityRec& r = world.entities.rec(e);
    if (r.kind == sema::EntityKind::class_ || r.kind == sema::EntityKind::data_member || r.kind == sema::EntityKind::type_alias ||
        r.kind == sema::EntityKind::enum_ || r.kind == sema::EntityKind::enumerator ||
        r.kind == sema::EntityKind::function_template || r.kind == sema::EntityKind::variable_template ||
        r.kind == sema::EntityKind::alias_template || r.kind == sema::EntityKind::concept_)
      return;  // no symbol: a template's specializations have them
    bool fn = r.kind != sema::EntityKind::variable;
    std::string id(ins.names.spelling(r.name));
    if (fn && id == "main" && r.scope == EntityId::none) tu.main = e;  // [basic.start.main]: the global one
    tu.entities.push_back({e, std::move(id), d.type, fn, r.lang, r.linkage, false, at, r.scope,
                           r.kind == sema::EntityKind::constructor  ? Special::constructor
                           : r.kind == sema::EntityKind::destructor ? Special::destructor
                                                                    : Special::none});
    tu.entities.back().asm_label = program.asm_label(e);
    if (r.templ != EntityId::none) {
      tu.entities.back().template_type = world.entities.rec(r.templ).type;
      tu.entities.back().template_args = r.args;
    }
  };
  for (std::uint32_t k = 0; k < items.items.size(); ++k) {
    const items::Item& it = items.items[k];
    // Items of the global namespace, and the members of its classes.
    bool member = it.parent != items::none && items.items[it.parent].kind == items::ItemKind::class_def &&
                  program.in_global_scope(it.parent);
    if (!program.in_global_scope(k) && !member) continue;
    // A class template's members are patterns; its specializations' are instances, below.
    if (member && items.items[it.parent].template_depth > 0) continue;
    if (it.kind != items::ItemKind::declaration && it.kind != items::ItemKind::function &&
        it.kind != items::ItemKind::class_def && it.kind != items::ItemKind::enum_def)  // its declarators: `struct S { … } s;`
      continue;
    const ast::ItemSema& is = program.item_sema(k).value;
    std::vector<ast::DeclSema> decls = ins.seqs.elements(is.decls);
    // The declarators, by declaration ordinal: their constants and item-relative locations (the
    // group is at the item's start).
    std::vector<std::optional<ast::Declarator>> declarators(decls.size());
    std::vector<std::optional<ast::Loc>> at(decls.size());
    if (is.tree != ast::NodeId::none && ins.ast.is<ast::DeclGroup>(is.tree)) {
      for (ast::Child c : ins.ast.children(ins.ast.get<ast::DeclGroup>(is.tree).declarators)) {
        auto dr = ins.ast.get<ast::Declarator>(c.id);
        if (dr.decl >= decls.size()) continue;
        declarators[dr.decl] = dr;
        at[dr.decl] = ast::Loc{program.id(k), c.at};
      }
    }
    for (std::size_t i = 0; i < decls.size(); ++i) declare(decls[i], at[i]);
    // A template's definition, or a class template's member defined outside it, is not code; its
    // instances are, below.
    if (!decls.empty() && ((decls[0].flags & sema::decl_flag::template_) || ins.seqs.size(decls[0].template_params) > 0))
      continue;
    if (it.kind == items::ItemKind::function && it.has_body() && !decls.empty() && decls[0].entity != EntityId::none &&
        !(decls[0].flags & sema::decl_flag::deleted)) {
      tu.functions.push_back({decls[0].entity, decls[0], program.analyze_body(k).value, program.id(k), it.body_begin - it.begin,
                              program.this_type(decls[0])});
      continue;
    }
    // Variable definitions, with the constants their declarators were initialized with.
    for (std::size_t i = 0; i < decls.size(); ++i) {
      const ast::DeclSema& d = decls[i];
      if (!declarators[i] || d.kind != ast::DeclKind::variable || d.entity == EntityId::none ||
          !(d.flags & sema::decl_flag::definition))
        continue;
      tu.variables.push_back({d.entity, d.type, declarators[i]->value, at[i]});
    }
  }
  // The function template specializations instantiated at the end of the unit ([temp.inst]), in
  // order of instantiation. A specialization that is only declared here is an external reference.
  for (const parse::Program::Instance* in : program.instances()) {
    if (in->failed || in->body.value.tree == ast::NodeId::none) continue;
    const items::Item& it = items.items[in->item];
    declare(in->decl, ast::Loc{program.id(in->item), 0});  // errors in its name point at the template
    tu.functions.push_back({in->spec, in->decl, in->body.value, program.id(in->item), it.body_begin - it.begin,
                            program.this_type(in->decl)});
  }
  // The variable template specializations instantiated ([temp.inst]/7), with their constants.
  for (const parse::Program::VarInstance& v : program.variable_instances()) {
    if (!v.cell.diags.empty()) continue;
    const ast::ItemSema& is = v.cell.value;
    for (std::size_t i = 0; i < ins.seqs.size(is.decls); ++i) {
      ast::DeclSema d = ins.seqs.at(is.decls, i);
      if (d.entity != v.spec) continue;
      ast::Loc at{program.id(v.item), 0};
      declare(d, at);
      ast::ConstId value = ast::ConstId::none;
      if (is.tree != ast::NodeId::none && ins.ast.is<ast::DeclGroup>(is.tree))
        for (ast::Child c : ins.ast.children(ins.ast.get<ast::DeclGroup>(is.tree).declarators))
          if (auto dr = ins.ast.get<ast::Declarator>(c.id); dr.decl == i) value = dr.value;
      if (d.flags & sema::decl_flag::definition) tu.variables.push_back({d.entity, d.type, value, at});
    }
  }
  // The static data members of class template specializations that are defined in the class
  // (constexpr or inline, [class.static.data]/4) and used, with their constants.
  for (const parse::Program::MemberInstance& m : program.member_instances()) {
    if (!m.cell->diags.empty() || world.types.is_dependent(world.types.class_type(m.spec))) continue;
    const ast::ItemSema& is = m.cell->value;
    if (is.tree == ast::NodeId::none || !ins.ast.is<ast::DeclGroup>(is.tree)) continue;
    for (ast::Child c : ins.ast.children(ins.ast.get<ast::DeclGroup>(is.tree).declarators)) {
      auto dr = ins.ast.get<ast::Declarator>(c.id);
      if (dr.decl >= ins.seqs.size(is.decls)) continue;
      ast::DeclSema d = ins.seqs.at(is.decls, dr.decl);
      if (d.kind != ast::DeclKind::variable || d.entity == EntityId::none || !(d.flags & sema::decl_flag::definition)) continue;
      // One whose initializer waits for a use is defined when it was used ([temp.inst]/3).
      ast::ConstId value = dr.value;
      if (program.is_lazy_member(d.entity)) {
        if (!std::ranges::contains(program.used_members(), d.entity)) continue;
        value = program.member_init(d.entity);
      }
      ast::Loc at{program.id(m.item), c.at};
      declare(d, at);
      tu.variables.push_back({d.entity, d.type, value, at});
    }
  }
  for (EntityId spec : program.required()) {
    ast::DeclSema d{};
    d.entity = spec;
    d.type = world.entities.rec(spec).type;
    declare(d, std::nullopt);
  }
  // The implicitly defined special members that are not trivial ([class.copy.ctor], [class.dtor]).
  for (std::size_t i = 0; i < program.synthesized().size(); ++i) {
    const parse::Program::Synthesized& s = program.synthesized()[i];
    // A class template's current instantiation is not code ([temp.dep.type]).
    if (world.types.is_dependent(world.types.class_type(world.entities.rec(s.decl.entity).scope))) continue;
    declare(s.decl, std::nullopt);
    tu.functions.push_back({s.decl.entity, s.decl, program.synthesized_body(i), program.id(s.item), 0, program.this_type(s.decl)});
  }
  return tu;
}

engine::query_task<FrontResult> TuFront::run(engine::Ctx& cx, TuId tu) {
  const TuSource& src = cx.fetch<TuSourceIn>(tu);
  contract_assert(src.file.has_value());  // a unit given as definitions has no front end
  const pp::FileId file{*src.file};
  sema::World& world = *universe().world;
  const pp::Region& region = co_await cx.fetch<pp::Pp>(file);
  const items::Items& items = co_await cx.fetch<items::RegionItems>(file);
  const std::vector<engine::Diagnostic>& lexed = co_await cx.fetch<engine::DiagsOf>(cx.cell_of<items::RegionItems>(file));
  FrontResult out;
  // `file:line:col` of a byte offset in an included file (region file `f`), rendered from the
  // file's text as a recorded input, so an edit that moves only its newlines re-renders the
  // position (#93). Phase 4's diagnostics (`kind`) may have a presumed line ([cpp.line]).
  auto where_in = [&](std::uint16_t f, std::uint32_t offset, std::optional<lex::LexDiag> kind) {
    const std::string& name = region.files[f];
    const pp::Source& source = cx.fetch<pp::SourceOf>(file);
    pp::Session& ps = pp::session_for(source.config);
    std::lock_guard lock(ps.mutex);
    std::string_view text;  // `<built-in>` is the session's
    if (name.starts_with('<')) {
      if (const pp::Files::Content* c = ps.files.content(ps.files.id(name))) text = c->text;
    } else {
      std::filesystem::path p(name);
      if (p.is_relative() && !source.cwd.empty()) p = source.cwd / p;
      const pp::PathId id = pp::path_id(p.lexically_normal().generic_native_encoded_string());
      // The editor view reads an open buffer before the file (daemon design §6.1).
      const std::optional<std::string>* content = nullptr;
      if (source.config.view == pp::View::editor) content = &cx.fetch<pp::Buffer>(id);
      if (!content || !*content) content = &cx.fetch<pp::FileContent>(id);
      if (*content) text = **content;
    }
    if (!kind) {
      LineColumn lc = LineTable(text).line_column(offset);
      return std::format("{}:{}:{}", region.file_names[f], lc.line, lc.column);
    }
    pp::Message pm = pp::message(ps.spellings, text, region.file_names[f], pp::Diag{f, offset, *kind, {}, 0, std::nullopt});
    return std::format("{}:{}:{}", pm.file, pm.line, pm.column);
  };
  // `pp` reports at byte offsets in the file (`lex::file_level`).
  for (const engine::Diagnostic& d : lexed) {
    auto kind = lex::lex_diag_of(d.id);
    if (!kind) continue;
    std::vector<std::uint64_t> args;
    for (const engine::DiagArg& a : d.args) args.push_back(a.raw);
    Message m{d.at.offset, lex::message_of(*kind).severity == lex::Severity::error, lex::render(*kind, args), {}};
    // Located in an included file (pp::file_loc).
    const auto f = static_cast<std::uint16_t>(lex::file_level - d.at.item);
    if (f != 0) m.where = where_in(f, d.at.offset, *kind);
    // The first token at or after the position in its file, else (a header of only directives)
    // the first token of a file included after it (files are numbered as they are first met).
    m.token = static_cast<std::uint32_t>(region.locs.size() - 1);
    std::uint32_t later = m.token;
    for (std::uint32_t t = 0; t + 1 < region.locs.size(); ++t) {
      pp::TokLoc l = region.site(t);
      if (l.file == f && l.offset >= d.at.offset) {
        m.token = t;
        break;
      }
      if (l.file > f && later == region.locs.size() - 1) later = t;
    }
    m.token = std::min(m.token, later);
    out.messages.push_back(std::move(m));
  }
  // The unit's language options: the instantiation limit (`-ftemplate-depth`).
  const pp::Source& unit_source = cx.fetch<pp::SourceOf>(file);
  parse::Program program(world, region, items, unit_source.config.template_depth);
  program.analyze_all();
  // What an editor's position requests read (#170): where each entity is first declared, and the
  // bodies, whether or not the unit has errors; only what the analysis reached, which analysing
  // more would change (its diagnostics).
  {
    std::set<std::uint32_t> seen;
    for (std::uint32_t k = 0; k < items.items.size(); ++k) {
      const items::Item& it = items.items[k];
      const ast::ItemSema* analysed = program.analysed_item(k);
      if (!analysed) continue;
      const ast::ItemSema& is = *analysed;
      const std::vector<ast::DeclSema> decls = world.ins.seqs.elements(is.decls);
      std::vector<std::uint32_t> at(decls.size(), 0);
      if (is.tree != ast::NodeId::none && world.ins.ast.is<ast::DeclGroup>(is.tree))
        for (ast::Child c : world.ins.ast.children(world.ins.ast.get<ast::DeclGroup>(is.tree).declarators)) {
          auto dr = world.ins.ast.get<ast::Declarator>(c.id);
          if (dr.decl < decls.size()) at[dr.decl] = c.at;
        }
      for (std::size_t i = 0; i < decls.size(); ++i)
        if (decls[i].entity != EntityId::none && seen.insert(std::to_underlying(decls[i].entity)).second)
          out.declared.emplace_back(decls[i].entity, ast::Loc{program.id(k), at[i]});
      if (const ast::BodySema* body = program.analysed_body(k); body && body->tree != ast::NodeId::none && it.has_body())
        out.bodies.push_back({program.id(k), it.decl_length(), body->tree});
    }
  }
  for (const engine::Diagnostic& d : program.diagnostics()) {
    auto kind = sema::sema_diag_of(d.id);
    if (!kind) continue;
    std::vector<std::uint64_t> args;
    for (const engine::DiagArg& a : d.args) args.push_back(a.raw);
    const pp::TokLoc at = program.site(d.at);
    out.messages.push_back({at.offset, sema::message_of(*kind).severity == lex::Severity::error,
                            sema::render(*kind, args, [&](char how, std::uint64_t raw) { return program.spell(how, raw); }),
                            // A declaration of an included header: located there.
                            at.file != 0 ? where_in(at.file, at.offset, std::nullopt) : std::string(), program.token(d.at)});
  }
  std::ranges::stable_sort(out.messages, {}, &Message::token);
  if (!out.has_errors()) out.unit = unit_of(program, world, items, src.name);
  // The unit's own records of its classes and enumerations (#115).
  const sema::Classes& classes = program.classes();
  for (EntityId c : classes.classes()) {
    ClassShape& shape = out.shapes.classes.emplace_back(ClassShape{c, {}});
    for (const sema::FieldRec& f : classes.rec(c).fields) shape.fields.push_back(f.type);
    shape.union_ = classes.rec(c).union_;
    shape.trivial_calls = classes.trivial_for_calls(c, unit_source.config.calls());
  }
  for (EntityId e : classes.enums()) out.shapes.enums.push_back({e, classes.underlying(e)});
  co_return out;
}

// A unit given as definitions has no classes of its own.
engine::query_task<Shapes> UnitShapes::run(engine::Ctx& cx, TuId tu) {
  if (!cx.fetch<TuSourceIn>(tu).file) co_return Shapes{};
  const FrontResult& u = co_await cx.fetch<TuFront>(tu);
  co_return u.shapes;
}

engine::query_task<TranslationUnit> TuUnit::run(engine::Ctx& cx, TuId tu) {
  const TuSource& src = cx.fetch<TuSourceIn>(tu);
  if (!src.file) co_return src.given;
  const FrontResult& front = co_await cx.fetch<TuFront>(tu);
  if (front.has_errors()) {
    TranslationUnit failed;
    failed.name = src.name;
    failed.front_failed = true;
    co_return failed;
  }
  co_return front.unit;
}

engine::query_task<std::optional<FileOffset>> DefinitionAt::run(engine::Ctx& cx, DefinitionKey k) {
  const pp::FileId file{k.tu.n};
  const pp::Source& source = cx.fetch<pp::SourceOf>(file);
  const pp::Region& region = co_await cx.fetch<pp::Pp>(file);
  const items::Items& items = co_await cx.fetch<items::RegionItems>(file);
  const FrontResult& front = co_await cx.fetch<TuFront>(k.tu);
  ast::Interners& in = *universe().in;
  auto absolute = [&](const std::string& name) {
    std::filesystem::path p(name);
    if (p.is_relative() && !source.cwd.empty()) p = std::filesystem::path(source.cwd) / p;
    return p.lexically_normal().generic_native_encoded_string();
  };
  // The region's number for the file, and the token at the offset: the last whose site in that
  // file starts at or before it (a macro's tokens all sit at its name).
  const std::string& want = pp::path_of(k.file);
  std::optional<std::uint16_t> f;
  for (std::size_t i = 0; i < region.files.size() && !f; ++i)
    if (!region.files[i].starts_with('<') && absolute(region.files[i]) == want) f = static_cast<std::uint16_t>(i);
  if (!f) co_return std::nullopt;
  std::optional<std::uint32_t> tok;
  for (std::uint32_t t = 0; t + 1 < region.locs.size(); ++t) {
    const pp::TokLoc l = region.site(t);
    if (l.file != *f || l.offset > k.offset) continue;
    if (!tok || region.site(*tok).offset < l.offset) tok = t;
  }
  // A name: an identifier the cursor is on (a macro's tokens sit at its name, of another length).
  if (!tok || region.tokens[*tok].kind != lex::Tk::identifier) co_return std::nullopt;
  if (region.locs[*tok].expansion == 0 && k.offset >= region.locs[*tok].offset + region.name(region.tokens[*tok]).size())
    co_return std::nullopt;
  // The innermost item holding it (items are in preorder), and the body that is that item's.
  std::optional<std::uint32_t> item;
  for (std::uint32_t i = 0; i < items.items.size(); ++i)
    if (items.items[i].begin <= *tok && *tok < items.items[i].end) item = i;
  if (!item) co_return std::nullopt;
  const ast::ItemId id = in.items.add<ast::ItemId>(items.items[*item].id);
  const std::uint32_t at = *tok - items.items[*item].begin;
  EntityId named = EntityId::none;
  for (const BodyAt& b : front.bodies) {
    if (b.item != id) continue;
    // The innermost node, through the nodes that wrap a name without being one (an implicit
    // conversion, parentheses); a call names its function only from its callee.
    ast::Red(in.ast, b.tree, b.offset).find(at, [&](const ast::Red& r) {
      const ast::Red* from = nullptr;
      for (const ast::Red* n = &r; n; from = n, n = n->parent()) {
        if (n->is<ast::DeclRef>()) named = n->get<ast::DeclRef>().entity;
        else if (n->is<ast::Member>()) named = n->get<ast::Member>().member;
        else if (n->is<ast::Call>() && from && from->id() == n->get<ast::Call>().callee.id) named = n->get<ast::Call>().function;
        else if (n->is<ast::ImplicitConversion>() || n->is<ast::Paren>()) continue;
        break;
      }
    });
  }
  if (named == EntityId::none) co_return std::nullopt;
  // A function template's specialization is declared as its template.
  auto d = std::ranges::find(front.declared, named, &std::pair<EntityId, ast::Loc>::first);
  if (d == front.declared.end())
    if (EntityId templ = universe().world->entities.rec(named).templ; templ != EntityId::none) {
      named = templ;
      d = std::ranges::find(front.declared, named, &std::pair<EntityId, ast::Loc>::first);
    }
  if (d == front.declared.end()) co_return std::nullopt;
  // Where it is first declared: its declarator's token in its item, moved to the declared name
  // (`*p` and `(*f)()` start before it), then that token's site.
  const std::string_view name = in.names.spelling(universe().world->entities.rec(named).name);
  for (const items::Item& it : items.items) {
    if (in.items.add<ast::ItemId>(it.id) != d->second.item) continue;
    std::size_t t = std::min<std::size_t>(it.begin + d->second.offset, region.locs.size() - 1);
    for (std::size_t u = t; u < it.end && u + 1 < region.tokens.size(); ++u)
      if (region.tokens[u].kind == lex::Tk::identifier && region.name(region.tokens[u]) == name) {
        t = u;
        break;
      }
    const pp::TokLoc l = region.site(t);
    if (region.files[l.file].starts_with('<')) co_return std::nullopt;
    co_return FileOffset{absolute(region.files[l.file]), l.offset};
  }
  co_return std::nullopt;
}

}  // namespace ycxx::pipeline
