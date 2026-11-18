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

export namespace ycxx::pipeline {

// A diagnostic ready to render: a byte offset in the file, its severity and its message.
struct Message {
  std::uint32_t offset = 0;
  bool error = true;
  std::string text;
  // `file:line:col` when the diagnostic is in another file than the main one (an included
  // header); the offset is then meaningless. Until #34's renderer.
  std::string where;
};

struct FrontResult {
  std::vector<Message> messages;  // in order of position
  TranslationUnit unit;           // meaningful when there are no errors
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
    if (r.kind == sema::EntityKind::class_ || r.kind == sema::EntityKind::data_member ||
        r.kind == sema::EntityKind::function_template)
      return;  // no symbol: a template's specializations have them
    bool fn = r.kind != sema::EntityKind::variable;
    std::string id(ins.names.spelling(r.name));
    if (fn && id == "main" && r.scope == EntityId::none) tu.main = e;  // [basic.start.main]: the global one
    tu.entities.push_back({e, std::move(id), d.type, fn, r.lang, r.linkage, false, at, r.scope,
                           r.kind == sema::EntityKind::constructor  ? Special::constructor
                           : r.kind == sema::EntityKind::destructor ? Special::destructor
                                                                    : Special::none});
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
    if (it.kind != items::ItemKind::declaration && it.kind != items::ItemKind::function &&
        it.kind != items::ItemKind::class_def)  // its declarators: `struct S { … } s;`
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
    // A template's definition is not code; its instances are, below.
    if (!decls.empty() && (decls[0].flags & sema::decl_flag::template_)) continue;
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
    tu.functions.push_back({in->spec, in->decl, in->body.value, program.id(in->item), it.body_begin - it.begin, std::nullopt});
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
  // `pp` reports at byte offsets in the file (`lex::file_level`).
  for (const engine::Diagnostic& d : lexed) {
    auto kind = lex::lex_diag_of(d.id);
    if (!kind) continue;
    std::vector<std::uint64_t> args;
    for (const engine::DiagArg& a : d.args) args.push_back(a.raw);
    Message m{d.at.offset, lex::message_of(*kind).severity == lex::Severity::error, lex::render(*kind, args), {}};
    // Located in an included file: rendered here (pp::file_loc), from the file's text as a
    // recorded input, so an edit that moves only its newlines re-renders the position (#93).
    if (d.at.item != lex::file_level) {
      std::uint16_t f = static_cast<std::uint16_t>(lex::file_level - d.at.item);
      const pp::Diag diag{f, d.at.offset, *kind, {}, 0, std::nullopt};
      const std::string& name = region.files[f];
      const pp::Source& source = cx.fetch<pp::SourceOf>(file);
      pp::Session& ps = pp::session_for(source.config);
      pp::Message pm;
      if (name.starts_with('<')) {
        pm = pp::message(ps, ps.files.id(name), region.file_names[f], diag);  // `<built-in>`
      } else {
        std::filesystem::path p(name);
        if (p.is_relative() && !source.cwd.empty()) p = source.cwd / p;
        const std::optional<std::string>& text = cx.fetch<pp::FileContent>(pp::path_id(p.lexically_normal().generic_native_encoded_string()));
        pm = pp::message(ps.spellings, text ? std::string_view(*text) : std::string_view(), region.file_names[f], diag);
      }
      m.where = std::format("{}:{}:{}", pm.file, pm.line, pm.column);
    }
    out.messages.push_back(std::move(m));
  }
  parse::Program program(world, region, items);
  program.analyze_all();
  for (const engine::Diagnostic& d : program.diagnostics()) {
    auto kind = sema::sema_diag_of(d.id);
    if (!kind) continue;
    std::vector<std::uint64_t> args;
    for (const engine::DiagArg& a : d.args) args.push_back(a.raw);
    out.messages.push_back({program.byte_offset(d.at), sema::message_of(*kind).severity == lex::Severity::error,
                            sema::render(*kind, args, [&](char how, std::uint64_t raw) { return program.spell(how, raw); }), {}});
  }
  std::ranges::stable_sort(out.messages, {}, &Message::offset);
  if (!out.has_errors()) out.unit = unit_of(program, world, items, src.name);
  co_return out;
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

}  // namespace ycxx::pipeline
