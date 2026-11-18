// A compilation session (#63): the engine and everything its cells' values point into (the
// interners, sema's world, the optimization units), alive together. `ycxx cxx` builds through
// one in-process; the daemon (#96) keeps one alive between requests, so a second build of an
// unchanged unit executes nothing past its inputs.
//
//   Session s;
//   s.set_source(TuId{0}, "a.cpp", text, "a.o");
//   s.set_program(ProgramId{0}, {{TuId{0}}, {}, "a.out"});
//   const Linked& l = s.get<Link>(ProgramId{0});
export module ycxx.pipeline:session;

import std;
import ycxx.ast;
import ycxx.engine;
import ycxx.items;
import ycxx.pp;
import ycxx.sema;
import :back;
import :front;
import :executable;

export namespace ycxx::pipeline {

class Session {
 public:
  // A session whose queries run on `pool`'s workers, each bound to this session's universe (#137).
  // The pool serves this session only, for as long as it lives.
  explicit Session(engine::Pool& pool) : db_(pool), pool_(&pool) {
    scope_ = {this, [](void* self) noexcept { bind_thread(&static_cast<Session*>(self)->universe_); },
              [] noexcept { bind_thread(nullptr); }};
    pool.claim_scope(&scope_);
    init();
  }
  Session() { init(); }
  ~Session() {
    if (pool_) pool_->release_scope(&scope_);
  }

 private:
  void init() {
    constexpr obj::target t = obj::target::aarch64_macos;
    db_.set<CostModelIn>(t, {});
    db_.set<OptCacheSnapshot>(t, {});
    db_.set<OptLevel>(t, 0);
    // Headers are read from the disk unless the owner provides them otherwise (the daemon's file
    // table, daemon design §5.1).
    db_.set_provider<pp::FileContent>(pp::read_disk);
  }

 public:
  Session(const Session&) = delete;

  // A translation unit from a source file: `path` names it (and locates its `#include "..."`),
  // `object` is the object's name; relative paths are relative to `cwd`, and `config` is its
  // preprocessing configuration. The driver's file number is the unit's.
  void set_source(TuId tu, std::string path, std::string text, std::string object, std::string cwd = {},
                  pp::Config config = {}) {
    Bind bind(universe_);  // input values may hold handles, fingerprinted by their interners
    const pp::FileId file{tu.n};
    db_.set<pp::SourceOf>(file, {std::move(path), std::move(cwd), std::move(config)});
    db_.set<pp::FileText>(file, std::move(text));
    db_.set<TuSourceIn>(tu, {std::move(object), tu.n, {}});
  }
  void set_program(ProgramId p, ProgramInputs in) {
    Bind bind(universe_);
    db_.set<ProgramIn>(p, std::move(in));
  }

  // A root request, with the session's universe bound.
  template <engine::MemoQuery Q>
  decltype(auto) get(const typename Q::Key& k) {
    Bind bind(universe_);
    return db_.get<Q>(k);
  }

  // The byte offset of a location in a source unit's main file.
  std::uint32_t byte_offset(TuId tu, ast::Loc at) {
    const pp::FileId file{tu.n};
    return pipeline::byte_offset(ins_, get<pp::Pp>(file), get<items::RegionItems>(file), at);
  }

  engine::Db& db() { return db_; }
  ast::Interners& interners() { return ins_; }

 private:
  ast::Interners ins_;
  sema::World world_{ins_};
  ValueConstants consts_{world_};
  UnitStore units_;
  Universe universe_{&ins_, &consts_, &ast::aarch64_darwin, &world_, &units_};
  engine::Db db_;
  engine::Pool* pool_ = nullptr;
  engine::Pool::Scope scope_;
};

}  // namespace ycxx::pipeline
