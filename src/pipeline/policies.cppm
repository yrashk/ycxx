// The opt-in local ownership policies over typed body occurrences (docs/design/policies-ownership.md).
module ycxx.pipeline:policies;

import std;
import ycxx.ast;
import ycxx.engine;
import ycxx.items;
import ycxx.pp;
import ycxx.sema;
import :back;
import :front;
import :lines;
import :ownership_policy;

namespace ycxx::pipeline {

// One checked build's ordered input units, shared by all its body queries.
using OwnershipSources = ast::SeqId<ast::u32>;

// Body-relative offsets let the engine reuse the finding if the whole function moves.
struct OwnershipBody {
  OwnershipSources sources;
  TuId tu;
  EntityId function;
  QualType type;
  ast::NodeId tree;
  ast::SeqId<ast::LocalRec> locals;
  QualType this_type{};  // a pointer type for nonstatic members
  bool linux_target;
  bool operator==(const OwnershipBody&) const = default;
};

struct OwnershipFinding {
  ast::u32 offset;
  std::string message;
  std::optional<ast::u32> previous{};
  bool previous_was_transfer = false;
};

enum class OwnershipEvent { branch, initialize, observe, consume, transfer, leave, may_unwind, unsupported };

struct OwnershipFlow {
  using Point = ast::u32;
  static constexpr Point exit = ~Point{};

  struct Local {
    ast::NameId name;
    Ownership ownership;
    bool parameter;
  };

  struct Step {
    OwnershipEvent event;
    ast::u32 offset;
    Point next = exit;
    Point other = exit;
    ast::u32 local = 0;
    ast::u32 source = 0;
    bool throwing_destructor = false;
    std::string problem{};
  };

  std::vector<Local> locals;
  std::vector<Step> steps;
  Point entry = exit;
};

namespace {

using Flow = OwnershipFlow;
using Point = Flow::Point;
using Event = OwnershipEvent;
using Effect = OwnershipEffect;

class OwnershipFlowBuilder {
 public:
  OwnershipFlowBuilder(const sema::World& world, const OwnershipPolicy& policy, OwnershipBody body)
      : world_(world), policy_(policy), body_(body) {
    for (const auto& local : world_.ins.seqs.elements(body.locals)) {
      const auto kind = ownership(local.type);
      flow_.locals.push_back({local.name, kind, (local.flags & sema::local_flag::parameter) != 0});
      has_linear_ = has_linear_ || kind == Ownership::linear;
      has_owner_ = has_owner_ || mentions_owner(local.type);
    }
  }

  Flow build() {
    if (effect(body_.function) != Effect::unknown || resource_lifetime_body()) return {};
    const auto signature = world_.types.rec(body_.type);
    if (mentions_owner(signature.inner) || std::ranges::any_of(signature.params, [&](QualType param) {
          return mentions_owner(param) && ownership(param) == Ownership::ordinary;
        })) {
      flow_.entry = unsupported(0, "ownership checking requires by-value resource parameters and disallows resource returns");
      return std::move(flow_);
    }
    ast::Red root(world_.ins.ast, body_.tree);
    if (!has_owner_ && !owner_use(root)) return {};

    Scope parameters;
    for (ast::u32 i = 0; i < flow_.locals.size(); ++i)
      if (flow_.locals[i].parameter && flow_.locals[i].ownership != Ownership::ordinary)
        parameters.locals.push_back({i, false});
    const Point end = leave(&parameters, nullptr, Flow::exit, last_token(root));
    flow_.entry = lower(root, end, {&parameters, nullptr});
    return std::move(flow_);
  }

 private:
  struct Cleanup {
    ast::u32 local;
    bool throwing_destructor;
  };
  struct Scope {
    const Scope* outer = nullptr;
    std::vector<Cleanup> locals{};
  };
  struct Loop {
    const Scope* break_scope;
    Point on_break;
    const Scope* continue_scope;
    Point on_continue;
  };
  struct Context {
    const Scope* scope;
    const Loop* loop;
  };

  Ownership ownership(QualType type) const {
    return policy_.ownership(world_.types.canonical(type));
  }

  Effect effect(EntityId function) const {
    return policy_.effect(ownership_operation(world_, function, body_.tu.n));
  }

  // The registered resource's construction and cleanup implement the adapter's contract.
  bool resource_lifetime_body() const {
    if (body_.this_type == QualType{}) return false;
    const auto* function = world_.entities.find(body_.function);
    return function && (function->kind == sema::EntityKind::constructor || function->kind == sema::EntityKind::destructor) &&
           ownership(world_.types.rec(body_.this_type).inner) != Ownership::ordinary;
  }

  bool mentions_owner(QualType type) const {
    if (ownership(type) != Ownership::ordinary) return true;
    const auto t = world_.types.rec(type);
    using enum sema::TypeKind;
    if (t.kind == pointer || t.kind == reference || t.kind == array || t.kind == member_pointer)
      return mentions_owner(t.inner) || (t.kind == member_pointer && ownership(t.member_owner) != Ownership::ordinary);
    if (t.kind == function)
      return mentions_owner(t.inner) || std::ranges::any_of(t.params, [&](QualType p) { return mentions_owner(p); });
    return false;
  }

  // The red cursor distinguishes the source occurrences of shared AST nodes.
  std::optional<ast::u32> owner_use(const ast::Red& node) const {
    const bool here = ast::dispatch<ast::Nodes>(node.kind(), [&]<class T>(std::type_identity<T>) {
      const auto n = node.get<T>();
      if constexpr (requires { n.info; } || std::is_same_v<T, ast::StaticInit> ||
                    std::is_same_v<T, ast::MemberInit> || std::is_same_v<T, ast::DestroyMember>) {
        if constexpr (requires { n.info; }) return mentions_owner(n.info.type);
        else return mentions_owner(n.type);
      } else if constexpr (std::is_same_v<T, ast::LocalVar> || std::is_same_v<T, ast::StaticVar>) {
        return mentions_owner(world_.ins.seqs.at(body_.locals, n.local).type);
      } else return false;
    });
    if (here) return node.offset();
    std::optional<ast::u32> at;
    node.for_each_child([&](const ast::Red& child) { if (!at) at = owner_use(child); });
    return at;
  }

  ast::Red at(const ast::Red& parent, ast::Child child) const {
    return ast::Red(parent.ast(), child.id, parent.offset() + child.at);
  }

  ast::u32 last_token(const ast::Red& node) const {
    return node.end() - (node.width() != 0);
  }

  ast::Red unparenthesized(ast::Red node) const {
    while (node.is<ast::Paren>()) node = at(node, node.get<ast::Paren>().operand);
    return node;
  }

  std::optional<ast::u32> named_owner(ast::Red node) const {
    for (node = unparenthesized(node); node.is<ast::ImplicitConversion>(); node = unparenthesized(node)) {
      const auto cast = node.get<ast::ImplicitConversion>();
      if (ownership(cast.info.type) == Ownership::ordinary || cast.conversion != ast::ConversionKind::qualification)
        return std::nullopt;
      node = at(node, cast.operand);
    }
    if (node.is<ast::LocalRef>()) {
      const auto local = node.get<ast::LocalRef>().local;
      if (flow_.locals[local].ownership != Ownership::ordinary) return local;
    }
    return std::nullopt;
  }

  Effect direct_effect(const ast::Red& node, const ast::Call& call) const {
    if (call.callee.id == ast::NodeId::none) return Effect::unknown;
    const ast::Red callee = unparenthesized(at(node, call.callee));
    if (!callee.is<ast::DeclRef>() || callee.get<ast::DeclRef>().entity != call.function) return Effect::unknown;
    return effect(call.function);
  }

  std::optional<ast::u32> operation_argument(const ast::Red& node, const ast::Call& call) const {
    const auto args = node.ast().children(call.args);
    if (args.size() == 1) return named_owner(at(node, args.front()));
    return std::nullopt;
  }

  Point add(Flow::Step step) {
    const auto id = static_cast<Point>(flow_.steps.size());
    flow_.steps.push_back(std::move(step));
    return id;
  }

  Point unsupported(ast::u32 offset, std::string message) {
    return add({.event = Event::unsupported, .offset = offset, .problem = std::move(message)});
  }

  Point leave(const Scope* scope, const Scope* keep, Point next, ast::u32 offset) {
    if (scope == keep) return next;
    next = leave(scope->outer, keep, next, offset);
    for (const Cleanup& c : scope->locals)
      next = add({.event = Event::leave, .offset = offset, .next = next, .local = c.local,
                  .throwing_destructor = c.throwing_destructor});
    return next;
  }

  // Declarations directly in this scope. A backedge can reach the same local slot again after
  // its scope exits. Cleanup events for declarations that were skipped see an absent slot.
  void collect(Scope& scope, const ast::Red& node) const {
    if (node.is<ast::LocalVar>()) {
      const auto var = node.get<ast::LocalVar>();
      const bool throws = throwing_destructor(var.destructor);
      if (flow_.locals[var.local].ownership != Ownership::ordinary || throws) scope.locals.push_back({var.local, throws});
    } else if (node.is<ast::DeclStmt>() || node.is<ast::CondDecl>()) {
      node.for_each_child([&](const ast::Red& child) { collect(scope, child); });
    }
  }

  void collect(Scope& scope, const ast::Red& parent, ast::Child child) const {
    if (child.id != ast::NodeId::none) collect(scope, at(parent, child));
  }

  Point scoped(const ast::Red& node, ast::Child child, Point next, Context context) {
    if (child.id == ast::NodeId::none) return next;
    const ast::Red sub = at(node, child);
    Scope scope{context.scope};
    collect(scope, sub);
    return lower(sub, leave(&scope, context.scope, next, last_token(sub)), {&scope, context.loop});
  }

  Point lower_child(const ast::Red& node, ast::Child child, Point next, Context context) {
    return child.id == ast::NodeId::none ? next : lower(at(node, child), next, context);
  }

  Point sequence(const ast::Red& node, ast::ChildList children, Point next, Context context) {
    for (ast::Child child : std::views::reverse(node.ast().children(children)))
      next = lower_child(node, child, next, context);
    return next;
  }

  std::optional<bool> literal_condition(const ast::Red& node, ast::Child condition) const {
    if (condition.id == ast::NodeId::none) return true;  // an omitted `for` condition
    const ast::Red expr = unparenthesized(at(node, condition));
    if (!expr.is<ast::Literal>()) return std::nullopt;
    const auto literal = expr.get<ast::Literal>();
    if (!world_.types.is(literal.info.type, ast::Builtin::bool_) ||
        !world_.ins.values.is<ast::VInt>(literal.value)) return std::nullopt;
    return ast::int_bits(world_.ins, literal.value).lo != 0;
  }

  Point condition(const ast::Red& node, ast::Child expression, Point on_true, Point on_false, Context context) {
    if (const auto known = literal_condition(node, expression)) return *known ? on_true : on_false;
    const auto expr = unparenthesized(at(node, expression));
    if (expr.is<ast::OrderedBinary>()) {
      const auto binary = expr.get<ast::OrderedBinary>();
      using enum ast::OrderedOp;
      if (binary.op == comma || binary.op == logical_and || binary.op == logical_or) {
        const Point right = condition(expr, binary.rhs, on_true, on_false, context);
        if (binary.op == comma) return lower_child(expr, binary.lhs, right, context);
        return binary.op == logical_and ? condition(expr, binary.lhs, right, on_false, context)
                                         : condition(expr, binary.lhs, on_true, right, context);
      }
    }
    if (expr.is<ast::Unary>() && expr.get<ast::Unary>().op == ast::UnaryOp::not_)
      return condition(expr, expr.get<ast::Unary>().operand, on_false, on_true, context);
    if (expr.is<ast::Conditional>()) {
      const auto ternary = expr.get<ast::Conditional>();
      const Point then_ = condition(expr, ternary.then_, on_true, on_false, context);
      const Point else_ = condition(expr, ternary.else_, on_true, on_false, context);
      return condition(expr, ternary.condition, then_, else_, context);
    }
    const Point next = on_true == on_false ? on_true : add({Event::branch, expr.offset(), on_true, on_false});
    return lower(expr, next, context);
  }

  Point lower_if(const ast::Red& node, Point next, Context context) {
    const auto stmt = node.get<ast::If>();
    Scope scope{context.scope};
    collect(scope, node, stmt.init);
    collect(scope, node, stmt.condition);
    const Context inside{&scope, context.loop};
    const Point end = leave(&scope, context.scope, next, last_token(node));
    const Point then_ = scoped(node, stmt.then_, end, inside);
    const Point else_ = scoped(node, stmt.else_, end, inside);
    return lower_child(node, stmt.init, condition(node, stmt.condition, then_, else_, inside), inside);
  }

  // `for`'s initializer survives the loop. A condition declaration belongs to one iteration
  // and stays alive through the increment. `while` has the same shape with neither extra part.
  Point lower_loop(const ast::Red& node, ast::For stmt, Point next, Context context) {
    Scope init{context.scope};
    collect(init, node, stmt.init);
    Scope iteration{&init};
    collect(iteration, node, stmt.condition);
    const auto end = last_token(node);
    const Point head = add({Event::branch, node.offset()});
    const Point done = leave(&iteration, context.scope, next, end);
    const Point repeat = leave(&iteration, &init, head, end);
    const Point increment = lower_child(node, stmt.increment, repeat, {&iteration, context.loop});
    const Loop loop{context.scope, next, &iteration, increment};
    const Point body = scoped(node, stmt.body, increment, {&iteration, &loop});
    const Point entry = condition(node, stmt.condition, body, done, {&iteration, &loop});
    flow_.steps[head].next = entry;
    return lower_child(node, stmt.init, head, {&init, context.loop});
  }

  Point lower_do(const ast::Red& node, Point next, Context context) {
    const auto stmt = node.get<ast::Do>();
    const Point head = add({Event::branch, node.offset()});
    const Point check = condition(node, stmt.condition, head, next, context);
    const Loop loop{context.scope, next, context.scope, check};
    const Point body = scoped(node, stmt.body, check, {context.scope, &loop});
    flow_.steps[head].next = body;
    return head;
  }

  Point lower_local(const ast::Red& node, Point next, Context context) {
    const auto var = node.get<ast::LocalVar>();
    const auto owner = flow_.locals[var.local].ownership;
    if (owner != Ownership::ordinary && var.init.id != ast::NodeId::none) {
      const ast::Red initializer = unparenthesized(at(node, var.init));
      if (initializer.is<ast::Call>()) {
        const auto call = initializer.get<ast::Call>();
        const Effect operation = direct_effect(initializer, call);
        if (operation == Effect::issue) return add({Event::initialize, node.offset(), next, Flow::exit, var.local});
        if (operation == Effect::transfer)
          if (const auto source = operation_argument(initializer, call))
            return add({Event::transfer, initializer.offset(), next, Flow::exit, var.local, *source});
      }
    }
    if (owner != Ownership::ordinary)
      return unsupported(node.offset(), "an owner must be initialized by a registered factory or transfer from a named owner");
    const auto& locals = world_.ins.seqs;
    if (mentions_owner(locals.at(body_.locals, var.local).type))
      return unsupported(node.offset(), "resource aliases are outside the checked protocol");
    if (throwing_destructor(var.destructor)) next = add({Event::initialize, node.offset(), next, Flow::exit, var.local});
    return lower_child(node, var.init, next, context);
  }

  Point lower(const ast::Red& node, Point next, Context context) {
    if (node.is<ast::Compound>()) {
      const auto stmts = node.get<ast::Compound>().stmts;
      Scope scope{context.scope};
      for (ast::Child stmt : node.ast().children(stmts)) collect(scope, node, stmt);
      const Point end = leave(&scope, context.scope, next, last_token(node));
      return sequence(node, stmts, end, {&scope, context.loop});
    }
    if (node.is<ast::DeclStmt>()) return sequence(node, node.get<ast::DeclStmt>().decls, next, context);
    if (node.is<ast::LocalVar>()) return lower_local(node, next, context);
    if (node.is<ast::ExprStmt>()) return lower_child(node, node.get<ast::ExprStmt>().expr, next, context);
    if (node.is<ast::If>()) return lower_if(node, next, context);
    if (node.is<ast::While>()) {
      const auto stmt = node.get<ast::While>();
      return lower_loop(node, {{}, stmt.condition, {}, stmt.body}, next, context);
    }
    if (node.is<ast::For>()) return lower_loop(node, node.get<ast::For>(), next, context);
    if (node.is<ast::Do>()) return lower_do(node, next, context);
    if (node.is<ast::Break>())
      return leave(context.scope, context.loop->break_scope, context.loop->on_break, node.offset());
    if (node.is<ast::Continue>())
      return leave(context.scope, context.loop->continue_scope, context.loop->on_continue, node.offset());
    if (node.is<ast::Return>()) {
      const Point end = leave(context.scope, nullptr, Flow::exit, node.offset());
      return lower_child(node, node.get<ast::Return>().value, end, context);
    }
    if (node.is<ast::Throw>()) {
      const Point end = add({Event::may_unwind, node.offset()});
      return lower_child(node, node.get<ast::Throw>().operand, end, context);
    }
    if (node.is<ast::CondDecl>()) {
      const auto expr = node.get<ast::CondDecl>();
      return lower_child(node, expr.var, lower_child(node, expr.value, next, context), context);
    }
    if (node.is<ast::Conditional>()) {
      const auto expr = node.get<ast::Conditional>();
      const Point then_ = lower_child(node, expr.then_, next, context);
      const Point else_ = lower_child(node, expr.else_, next, context);
      return condition(node, expr.condition, then_, else_, context);
    }
    if (node.is<ast::OrderedBinary>()) {
      const auto expr = node.get<ast::OrderedBinary>();
      using enum ast::OrderedOp;
      if (expr.op == comma) return lower_child(node, expr.lhs, lower_child(node, expr.rhs, next, context), context);
      if (expr.op == logical_and || expr.op == logical_or) {
        const Point rhs = lower_child(node, expr.rhs, next, context);
        return expr.op == logical_and ? condition(node, expr.lhs, rhs, next, context)
                                       : condition(node, expr.lhs, next, rhs, context);
      }
    }
    if (node.is<ast::Call>()) {
      const auto call = node.get<ast::Call>();
      const auto operation = direct_effect(node, call);
      if (operation == Effect::observe || operation == Effect::consume) {
        if (const auto local = operation_argument(node, call))
          return add({operation == Effect::consume ? Event::consume : Event::observe, node.offset(), next, Flow::exit, *local});
        return unsupported(node.offset(), "an ownership protocol operation requires a named owner");
      }
    }
    if (const auto local = named_owner(node)) return add({Event::observe, node.offset(), next, Flow::exit, *local});
    if (node.is<ast::Paren>()) return lower_child(node, node.get<ast::Paren>().operand, next, context);
    if (node.is<ast::Goto>() || node.is<ast::Label>() || node.is<ast::Switch>() || node.is<ast::TryBlock>() ||
        node.is<ast::IfConsteval>()) return unsupported(node.offset(), "this control flow is outside the ownership prototype");
    if (const auto offset = owner_use(node)) return unsupported(*offset, "this resource use is outside the checked protocol");
    if (has_linear_)
      if (const auto throwing = throwing_site(node)) return add({Event::may_unwind, *throwing, next});
    return next;
  }

  bool nonthrowing_type(QualType type) const {
    auto fn = world_.types.rec(type);
    if (fn.kind == sema::TypeKind::pointer || fn.kind == sema::TypeKind::reference) fn = world_.types.rec(fn.inner);
    return fn.kind == sema::TypeKind::function && fn.fn_noexcept;
  }

  bool nonthrowing(EntityId function) const {
    const auto* entity = world_.entities.find(function);
    return entity && nonthrowing_type(entity->type);
  }

  bool throwing_destructor(EntityId function) const {
    return has_linear_ && function != EntityId::none && !nonthrowing(function);
  }

  std::optional<ast::u32> throwing_site(const ast::Red& node) const {
    const bool here = ast::dispatch<ast::Nodes>(node.kind(), [&]<class T>(std::type_identity<T>) {
      const auto expr = node.get<T>();
      if constexpr (std::is_same_v<T, ast::Call> || std::is_same_v<T, ast::MemberCall> || std::is_same_v<T, ast::AssignmentCall>)
        return !nonthrowing(expr.function);
      else if constexpr (std::is_same_v<T, ast::VirtualMemberCall>) return !nonthrowing_type(expr.callee);
      else if constexpr (std::is_same_v<T, ast::CtorCall>) return !nonthrowing(expr.ctor);
      else if constexpr (std::is_same_v<T, ast::ImplicitConversion>)
        return expr.conversion == ast::ConversionKind::user_defined && !nonthrowing(expr.function);
      else if constexpr (std::is_same_v<T, ast::Materialize>) return throwing_destructor(expr.destructor);
      else return std::is_same_v<T, ast::Throw> || std::is_same_v<T, ast::NewObject> || std::is_same_v<T, ast::NewArray> ||
                  std::is_same_v<T, ast::DeleteObject> || std::is_same_v<T, ast::DeleteArray>;
    });
    if (here) return node.offset();
    std::optional<ast::u32> offset;
    node.for_each_child([&](const ast::Red& child) { if (!offset) offset = throwing_site(child); });
    return offset;
  }

  const sema::World& world_;
  const OwnershipPolicy& policy_;
  OwnershipBody body_;
  Flow flow_;
  bool has_owner_ = false;
  bool has_linear_ = false;
};

class OwnershipAnalysis {
 public:
  OwnershipAnalysis(const ast::Interners& in, const Flow& flow) : in_(in), flow_(flow) {}

  std::optional<OwnershipFinding> run() const {
    if (flow_.entry == Flow::exit) return std::nullopt;
    std::vector<std::optional<State>> paths(flow_.steps.size());
    std::deque<Point> work;
    State entry;
    for (const auto& local : flow_.locals)
      entry.push_back({local.parameter && local.ownership != Ownership::ordinary ? live : absent});
    paths[flow_.entry] = std::move(entry);
    work.push_back(flow_.entry);
    auto propagate = [&](Point point, const State& state) {
      if (point == Flow::exit) return;
      auto& incoming = paths[point];
      if (!incoming) {
        incoming = state;
      } else {
        bool changed = false;
        for (std::size_t i = 0; i < state.size(); ++i) {
          auto& slot = (*incoming)[i];
          const auto merged = static_cast<Bits>(slot.possible | state[i].possible);
          changed = changed || slot.possible != merged;
          slot.possible = merged;
          if (!slot.spent_at && state[i].spent_at) {
            slot.spent_at = state[i].spent_at;
            slot.transferred = state[i].transferred;
          }
        }
        if (!changed) return;
      }
      work.push_back(point);
    };
    while (!work.empty()) {
      const auto point = work.front();
      work.pop_front();
      const auto& step = flow_.steps[point];
      State state = *paths[point];
      if (auto problem = apply(step, state)) return problem;
      propagate(step.next, state);
      if (step.event == Event::branch) propagate(step.other, state);
    }
    return std::nullopt;
  }

 private:
  using Bits = std::uint8_t;
  static constexpr Bits absent = 1, live = 2, spent = 4;
  struct Slot {
    Bits possible;
    std::optional<ast::u32> spent_at{};
    bool transferred = false;
  };
  using State = std::vector<Slot>;

  std::string description(ast::u32 local) const {
    const auto& owner = flow_.locals[local];
    const auto name = in_.names.spelling(owner.name);
    return std::format("{} '{}'", owner.ownership == Ownership::linear ? "linear resource" : "affine resource",
                       name.empty() ? "<unnamed>" : name);
  }

  std::optional<OwnershipFinding> require_live(const Flow::Step& step, ast::u32 local, const Slot& slot) const {
    if (slot.possible == live) return std::nullopt;
    if (slot.spent_at) {
      const auto end = slot.transferred ? " may already have transferred its obligation" : " may already have been consumed";
      return OwnershipFinding{step.offset, description(local) + end, slot.spent_at, slot.transferred};
    }
    return OwnershipFinding{step.offset, description(local) + " may be unavailable"};
  }

  std::optional<OwnershipFinding> may_unwind(ast::u32 offset, const State& state) const {
    for (ast::u32 i = 0; i < state.size(); ++i)
      if (flow_.locals[i].ownership == Ownership::linear && (state[i].possible & live))
        return OwnershipFinding{offset, "operation may unwind while " + description(i) + " is live"};
    return std::nullopt;
  }

  std::optional<OwnershipFinding> apply(const Flow::Step& step, State& state) const {
    if (step.event == Event::unsupported) return OwnershipFinding{step.offset, step.problem};
    if (step.event == Event::branch) return std::nullopt;
    if (step.event == Event::may_unwind) return may_unwind(step.offset, state);
    auto& slot = state[step.local];
    switch (step.event) {
    case Event::observe: return require_live(step, step.local, slot);
    case Event::consume:
      if (auto problem = require_live(step, step.local, slot)) return problem;
      slot = {spent, step.offset};
      break;
    case Event::transfer:
      if (auto problem = require_live(step, step.source, state[step.source])) return problem;
      state[step.source] = {spent, step.offset, true};
      [[fallthrough]];
    case Event::initialize:
      if (flow_.locals[step.local].ownership == Ownership::linear && (slot.possible & live))
        return OwnershipFinding{step.offset, "initialization would overwrite live " + description(step.local)};
      slot = {live};
      break;
    case Event::leave:
      if (flow_.locals[step.local].ownership == Ownership::linear && (slot.possible & live))
        return OwnershipFinding{step.offset, description(step.local) + " may leave its scope without being consumed or transferred"};
      if (step.throwing_destructor && (slot.possible & live))
        if (auto problem = may_unwind(step.offset, state)) return problem;
      slot = {absent};
      break;
    default: break;
    }
    return std::nullopt;
  }

  const ast::Interners& in_;
  const Flow& flow_;
};

}  // namespace

struct OwnershipDeclarationsFor {
  using Key = ast::u32;
  using Value = OwnershipDeclarations;
  static engine::query_task<Value> run(engine::Ctx& cx, Key unit) {
    const TuId tu{unit};
    const auto& front = co_await cx.fetch<TuFront>(tu);
    if (front.ownership_policies.empty() && front.ownership_annotations.empty()) co_return Value{};
    const auto& source = cx.fetch<pp::SourceOf>(pp::FileId{unit});
    co_return read_ownership_policies(*universe().world_for(*source.config.target), unit, front.ownership_policies,
                                     front.ownership_annotations);
  }
};

struct OwnershipRegistryFor {
  using Key = OwnershipSources;
  using Value = OwnershipRegistry;
  static engine::query_task<Value> run(engine::Ctx& cx, Key sources) {
    const auto units = universe().in->seqs.elements(sources);
    if (units.empty()) co_return Value{};
    const auto fragments = co_await cx.par_map<OwnershipDeclarationsFor>(units);
    const auto& source = cx.fetch<pp::SourceOf>(pp::FileId{units.front()});
    co_return compose_ownership_policies(*universe().world_for(*source.config.target), fragments);
  }
};

struct BuildOwnershipFlow {
  using Key = OwnershipBody;
  using Value = OwnershipFlow;
  static engine::query_task<Value> run(engine::Ctx& cx, Key body) {
    const auto& registry = co_await cx.fetch<OwnershipRegistryFor>(body.sources);
    const auto& target = body.linux_target ? ast::x86_64_linux : ast::aarch64_darwin;
    co_return OwnershipFlowBuilder(*universe().world_for(target), registry.policy, body).build();
  }
};

struct CheckOwnershipBody {
  using Key = OwnershipBody;
  using Value = std::optional<OwnershipFinding>;
  static engine::query_task<Value> run(engine::Ctx& cx, Key body) {
    const auto& flow = co_await cx.fetch<BuildOwnershipFlow>(body);
    co_return OwnershipAnalysis(*universe().in, flow).run();
  }
};

struct OwnershipCheckUnit {
  OwnershipSources sources;
  TuId tu;
  bool operator==(const OwnershipCheckUnit&) const = default;
};

struct CheckPolicies {
  using Key = OwnershipCheckUnit;
  using Value = std::vector<Message>;
  static engine::query_task<Value> run(engine::Ctx&, Key);
};

engine::query_task<CheckPolicies::Value> CheckPolicies::run(engine::Ctx& cx, Key key) {
  const TuId tu = key.tu;
  const FrontResult& front = co_await cx.fetch<TuFront>(tu);
  if (front.has_errors()) co_return Value{};
  const auto& registry = co_await cx.fetch<OwnershipRegistryFor>(key.sources);
  if (registry.policy.resources.empty() && registry.problems.empty()) co_return Value{};
  if (front.unit.virtual_emission_blocked && registry.problems.empty())
    co_return Value{{std::nullopt, true, "ownership analysis is unavailable for this unit's virtual declarations", {}}};
  std::span<const OwnershipPolicyProblem> problems;
  if (const auto it = registry.problems.find(tu.n); it != registry.problems.end()) problems = it->second;

  const pp::FileId file{tu.n};
  const pp::Source& source = cx.fetch<pp::SourceOf>(file);
  const bool linux_target = source.config.target->triple == ast::x86_64_linux.triple;
  std::vector<const FunctionDef*> definitions;
  std::vector<OwnershipBody> bodies;
  if (registry.problems.empty())
    for (const auto& def : front.unit.functions) {
      if (def.body.tree == ast::NodeId::none) continue;
      definitions.push_back(&def);
      bodies.push_back({key.sources, tu, def.entity, def.decl.type, def.body.tree, def.body.locals,
                        def.this_type.value_or(QualType{}), linux_target});
    }
  const auto findings = co_await cx.par_map<CheckOwnershipBody>(bodies);
  if (problems.empty() &&
      std::ranges::none_of(findings, [](const auto& finding) { return finding.get().has_value(); }))
    co_return Value{};

  const pp::Region& region = co_await cx.fetch<pp::Pp>(file);
  const items::Items& items = co_await cx.fetch<items::RegionItems>(file);
  SourceLocations locations(cx, source, file, region, items);
  auto& in = *universe().in;
  Value messages;
  std::map<ast::u32, SourceLocations> previous_units;
  for (const auto& problem : problems) {
    const auto at = locations.at(in, problem.origin.at);
    std::string text = problem.message;
    if (problem.previous) {
      const auto origin = *problem.previous;
      SourceLocations* previous_locations = &locations;
      if (origin.unit != tu.n) {
        auto it = previous_units.find(origin.unit);
        if (it == previous_units.end()) {
          const pp::FileId other{origin.unit};
          const auto& other_source = cx.fetch<pp::SourceOf>(other);
          const auto& other_region = co_await cx.fetch<pp::Pp>(other);
          const auto& other_items = co_await cx.fetch<items::RegionItems>(other);
          it = previous_units.try_emplace(origin.unit, cx, other_source, other, other_region, other_items).first;
        }
        previous_locations = &it->second;
      }
      if (const auto previous = previous_locations->at(in, origin.at))
        text += std::format(" (previous declaration at {})", previous->where);
    }
    messages.push_back({at ? std::optional(at->offset) : std::nullopt, true, std::move(text), at ? at->where : ""});
  }
  for (std::size_t i = 0; i < findings.size(); ++i) {
    const auto& result = findings[i].get();
    if (!result) continue;
    const auto& finding = *result;
    const auto& def = *definitions[i];
    const auto at = locations.at(in, {def.item, def.body_offset + finding.offset});
    std::string text = finding.message;
    if (finding.previous)
      if (const auto previous = locations.at(in, {def.item, def.body_offset + *finding.previous}))
        text += std::format(" (previous {} at {})", finding.previous_was_transfer ? "transfer" : "consumption", previous->where);
    messages.push_back({at ? std::optional(at->offset) : std::nullopt, true, std::move(text), at ? at->where : ""});
  }
  co_return messages;
}

}  // namespace ycxx::pipeline
