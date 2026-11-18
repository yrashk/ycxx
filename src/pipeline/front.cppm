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

  ast::Interners ins_;
  sema::World world_{ins_};
  ValueConstants consts_{world_};
  engine::Db db_;
  pp::Region region_;
  items::Items items_;
  std::unique_ptr<parse::Program> program_;
  std::vector<Message> messages_;
};

}  // namespace ycxx::pipeline

namespace ycxx::pipeline {

constexpr pp::FileId main_file{0};

Front::Front(std::string_view, std::string text) {
  db_.set<pp::FileText>(main_file, std::move(text));
  region_ = db_.get<pp::Pp>(main_file);
  items_ = db_.get<items::RegionItems>(main_file);
  // `pp` reports at byte offsets in the file (`lex::file_level`).
  for (const engine::Diagnostic& d : db_.diags_of<items::RegionItems>(main_file)) {
    auto kind = lex::lex_diag_of(d.id);
    if (!kind) continue;
    std::vector<std::uint64_t> args;
    for (const engine::DiagArg& a : d.args) args.push_back(a.raw);
    messages_.push_back({d.at.offset, lex::message_of(*kind).severity == lex::Severity::error, lex::render(*kind, args)});
  }
  program_ = std::make_unique<parse::Program>(world_, region_, items_);
  program_->analyze_all();
  for (const engine::Diagnostic& d : program_->diagnostics()) {
    auto kind = sema::sema_diag_of(d.id);
    if (!kind) continue;
    std::vector<std::uint64_t> args;
    for (const engine::DiagArg& a : d.args) args.push_back(a.raw);
    messages_.push_back({program_->byte_offset(d.at), sema::message_of(*kind).severity == lex::Severity::error,
                         sema::render(*kind, args, [&](char how, std::uint64_t raw) { return program_->spell(how, raw); })});
  }
  std::ranges::stable_sort(messages_, {}, &Message::offset);
}

TranslationUnit Front::unit(std::string name) {
  TranslationUnit tu;
  tu.name = std::move(name);
  std::set<std::uint32_t> seen;
  auto declare = [&](EntityId e) {
    if (e == EntityId::none || !seen.insert(std::to_underlying(e)).second) return;
    const sema::EntityRec& r = world_.entities.rec(e);
    bool fn = r.kind == sema::EntityKind::function;
    std::string id(ins_.names.spelling(r.name));
    if (fn && id == "main") tu.main = e;
    tu.entities.push_back({e, std::move(id), r.type, fn, r.lang, r.linkage, false});
  };
  for (std::uint32_t k = 0; k < items_.items.size(); ++k) {
    const items::Item& it = items_.items[k];
    if (!program_->in_global_scope(k)) continue;
    if (it.kind != items::ItemKind::declaration && it.kind != items::ItemKind::function) continue;
    const ast::ItemSema& is = program_->item_sema(k).value;
    std::vector<ast::DeclSema> decls = ins_.seqs.elements(is.decls);
    for (const ast::DeclSema& d : decls) declare(d.entity);
    if (it.kind == items::ItemKind::function && it.has_body() && !decls.empty() && decls[0].entity != EntityId::none) {
      tu.functions.push_back({decls[0].entity, decls[0], program_->analyze_body(k).value, program_->id(k),
                              it.body_begin - it.begin});
      continue;
    }
    // Variable definitions, with the constants their declarators were initialized with.
    if (is.tree == ast::NodeId::none || !ins_.ast.is<ast::DeclGroup>(is.tree)) continue;
    for (ast::Child c : ins_.ast.children(ins_.ast.get<ast::DeclGroup>(is.tree).declarators)) {
      auto dr = ins_.ast.get<ast::Declarator>(c.id);
      if (dr.decl >= decls.size()) continue;
      const ast::DeclSema& d = decls[dr.decl];
      if (d.kind != ast::DeclKind::variable || d.entity == EntityId::none || !(d.flags & sema::decl_flag::definition))
        continue;
      tu.variables.push_back({d.entity, d.type, dr.value});
    }
  }
  return tu;
}

}  // namespace ycxx::pipeline
