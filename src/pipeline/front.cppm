// The front half of a translation unit's pipeline (#43): the file's text as the engine's input,
// `pp` and the item splitter as engine queries, then the parser and sema (`item_sema`,
// `analyze_body`) through `parse::Program`, which plays the engine for them until contextual
// queries exist (#16). Its results are the diagnostics, located by byte offset, and the
// `TranslationUnit` the back half compiles.
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

// One translation unit, analysed. It owns everything the front end built, which the back half
// reads (the interners, sema's world); keep it alive while the unit is compiled.
class Front {
 public:
  Front(std::string_view path, std::string text);
  Front(const Front&) = delete;

  // Every diagnostic of the front end, in order of position.
  const std::vector<Message>& messages() const { return messages_; }
  bool has_errors() const {
    return std::ranges::any_of(messages_, [](const Message& m) { return m.error; });
  }

  // The definitions and declarations for the back half; meaningful when there are no errors.
  TranslationUnit unit(std::string name);

  // The byte offset of an item location (ast.md §8.3, steps 1–3), for the back half's errors.
  std::uint32_t byte_offset(ast::Loc at) const {
    return program_->byte_offset(engine::Loc{std::to_underlying(at.item), at.offset});
  }

  ast::Interners& interners() { return ins_; }
  Constants& constants() { return consts_; }
  const Records& records() const { return records_; }

 private:
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

  // Class layouts and names, from sema's class table.
  class ClassRecords final : public Records {
   public:
    explicit ClassRecords(sema::World& w) : w_(&w) {}
    std::optional<Record> record(EntityId c) const override {
      if (!w_->classes.is_defined(c)) return std::nullopt;
      QualType t = w_->types.class_type(c);
      sema::ObjectLayout l = w_->classes.layout(t);
      Record r{l.size, l.align, {}};
      const sema::ClassRec* cr = w_->classes.find(c);
      for (std::size_t i = 0; i < cr->fields.size(); ++i)
        r.fields.push_back({l.offsets[i], w_->ins.canonical(cr->fields[i].type)});
      return r;
    }
    QualType underlying(EntityId e) const override { return w_->classes.underlying(e); }
    EntityId scope(EntityId c) const override { return w_->entities.rec(c).scope; }
    std::string class_name(EntityId c) const override {
      return std::string(w_->ins.names.spelling(w_->entities.rec(c).name));
    }

   private:
    sema::World* w_;
  };

  ast::Interners ins_;
  sema::World world_{ins_};
  ValueConstants consts_{world_};
  ClassRecords records_{world_};
  engine::Db db_;
  pp::Region region_;
  items::Items items_;
  std::unique_ptr<parse::Program> program_;
  std::vector<Message> messages_;
};

}  // namespace ycxx::pipeline

namespace ycxx::pipeline {

constexpr pp::FileId main_file{0};

Front::Front(std::string_view path, std::string text) {
  pp::name_file(main_file, std::string(path));
  db_.set<pp::FileText>(main_file, std::move(text));
  region_ = db_.get<pp::Pp>(main_file);
  items_ = db_.get<items::RegionItems>(main_file);
  // `pp` reports at byte offsets in the file (`lex::file_level`).
  for (const engine::Diagnostic& d : db_.diags_of<items::RegionItems>(main_file)) {
    auto kind = lex::lex_diag_of(d.id);
    if (!kind) continue;
    std::vector<std::uint64_t> args;
    for (const engine::DiagArg& a : d.args) args.push_back(a.raw);
    Message m{d.at.offset, lex::message_of(*kind).severity == lex::Severity::error, lex::render(*kind, args), {}};
    // Located in an included file: rendered here (pp::file_loc).
    if (d.at.item != lex::file_level) {
      std::uint16_t f = static_cast<std::uint16_t>(lex::file_level - d.at.item);
      pp::Message pm = pp::message(pp::session(), pp::session().files.id(region_.files[f]), region_.file_names[f],
                                   pp::Diag{f, d.at.offset, *kind, {}});
      m.where = std::format("{}:{}:{}", pm.file, pm.line, pm.column);
    }
    messages_.push_back(std::move(m));
  }
  program_ = std::make_unique<parse::Program>(world_, region_, items_);
  program_->analyze_all();
  for (const engine::Diagnostic& d : program_->diagnostics()) {
    auto kind = sema::sema_diag_of(d.id);
    if (!kind) continue;
    std::vector<std::uint64_t> args;
    for (const engine::DiagArg& a : d.args) args.push_back(a.raw);
    messages_.push_back({program_->byte_offset(d.at), sema::message_of(*kind).severity == lex::Severity::error,
                         sema::render(*kind, args, [&](char how, std::uint64_t raw) { return program_->spell(how, raw); }), {}});
  }
  std::ranges::stable_sort(messages_, {}, &Message::offset);
}

TranslationUnit Front::unit(std::string name) {
  TranslationUnit tu;
  tu.name = std::move(name);
  std::set<std::uint32_t> seen;
  auto declare = [&](EntityId e, std::optional<ast::Loc> at) {
    if (e == EntityId::none || !seen.insert(std::to_underlying(e)).second) return;
    const sema::EntityRec& r = world_.entities.rec(e);
    if (r.kind == sema::EntityKind::class_ || r.kind == sema::EntityKind::data_member) return;
    bool fn = r.kind != sema::EntityKind::variable;
    std::string id(ins_.names.spelling(r.name));
    if (fn && id == "main") tu.main = e;
    tu.entities.push_back({e, std::move(id), r.type, fn, r.lang, r.linkage, false, at, r.scope,
                           r.kind == sema::EntityKind::constructor  ? Special::constructor
                           : r.kind == sema::EntityKind::destructor ? Special::destructor
                                                                    : Special::none});
  };
  for (std::uint32_t k = 0; k < items_.items.size(); ++k) {
    const items::Item& it = items_.items[k];
    // Items of the global namespace, and the members of its classes.
    bool member = it.parent != items::none && items_.items[it.parent].kind == items::ItemKind::class_def &&
                  program_->in_global_scope(it.parent);
    if (!program_->in_global_scope(k) && !member) continue;
    if (it.kind != items::ItemKind::declaration && it.kind != items::ItemKind::function &&
        it.kind != items::ItemKind::class_def)
      continue;
    const ast::ItemSema& is = program_->item_sema(k).value;
    std::vector<ast::DeclSema> decls = ins_.seqs.elements(is.decls);
    // The declarators, by declaration ordinal: their constants and item-relative locations (the
    // group is at the item's start).
    std::vector<std::optional<ast::Declarator>> declarators(decls.size());
    std::vector<std::optional<ast::Loc>> at(decls.size());
    if (is.tree != ast::NodeId::none && ins_.ast.is<ast::DeclGroup>(is.tree)) {
      for (ast::Child c : ins_.ast.children(ins_.ast.get<ast::DeclGroup>(is.tree).declarators)) {
        auto dr = ins_.ast.get<ast::Declarator>(c.id);
        if (dr.decl >= decls.size()) continue;
        declarators[dr.decl] = dr;
        at[dr.decl] = ast::Loc{program_->id(k), c.at};
      }
    }
    for (std::size_t i = 0; i < decls.size(); ++i) declare(decls[i].entity, at[i]);
    if (it.kind == items::ItemKind::function && it.has_body() && !decls.empty() && decls[0].entity != EntityId::none &&
        !(decls[0].flags & sema::decl_flag::deleted)) {
      tu.functions.push_back({decls[0].entity, decls[0], program_->analyze_body(k).value, program_->id(k),
                              it.body_begin - it.begin});
      tu.functions.back().this_type = program_->this_type(decls[0]);
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
  // The implicitly defined special members that are not trivial ([class.copy.ctor], [class.dtor]).
  for (std::size_t i = 0; i < program_->synthesized().size(); ++i) {
    const parse::Program::Synthesized& s = program_->synthesized()[i];
    declare(s.decl.entity, std::nullopt);
    tu.functions.push_back({s.decl.entity, s.decl, program_->synthesized_body(i), program_->id(s.item), 0});
    tu.functions.back().this_type = program_->this_type(s.decl);
  }
  return tu;
}

}  // namespace ycxx::pipeline
