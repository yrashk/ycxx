// Opt-in ownership and effect checks over typed body occurrences.
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
import :policy_registry;

namespace ycxx::pipeline {

// One checked build's ordered input units, shared by all its body queries.
using PolicySources = ast::SeqId<ast::u32>;

// Body-relative offsets let the engine reuse the finding if the whole function moves.
struct OwnershipBody {
  PolicySources sources;
  TuId tu;
  EntityId function;
  QualType type;
  ast::NodeId tree;
  ast::SeqId<ast::LocalRec> locals;
  QualType this_type{};  // a pointer type for nonstatic members
  bool linux_target;
  bool defer_observers;
  bool operator==(const OwnershipBody&) const = default;
};

enum class OwnershipEvent { branch, initialize, borrow, observe, consume, transfer, leave, may_unwind, unsupported };

struct OwnershipFinding {
  ast::u32 offset;
  std::string message;
  std::optional<ast::u32> previous{};
  OwnershipEvent previous_event = OwnershipEvent::consume;
};

struct OwnershipBodyCheck {
  std::optional<OwnershipFinding> finding;
  std::vector<EntityId> observers;
};

enum class PolicyContractKind { observation, effect, trusted_effect };

struct PolicyContract {
  EntityId function;
  EntityId target;
  ast::Fp128 signature;
  PolicyContractKind kind;
  bool provides;
};

struct OwnershipRecord {
  struct Field {
    ast::NameId name;
    Ownership mode;
    EntityId record = EntityId::none;
  };

  EntityId entity;
  std::vector<Field> fields;
  bool supported;
  EntityId destructor;
};

struct OwnershipRecords {
  std::vector<OwnershipRecord> records;
  std::vector<EntityId> nonthrowing_destructors;
};

struct OwnershipFlow {
  using Point = ast::u32;
  static constexpr Point exit = ~Point{};

  struct Borrow {
    ast::u32 owner;
    ast::u32 offset;
  };

  struct Local {
    ast::NameId name;
    Ownership ownership;
    bool parameter;
    bool shared_reference;
    std::optional<Borrow> borrow{};
    std::string field{};
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
    EntityId observer = EntityId::none;
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
  OwnershipFlowBuilder(const sema::World& world, const OwnershipPolicy& policy,
                       const OwnershipRecords& records, OwnershipBody body)
      : world_(world), policy_(policy), records_(records.records),
        nonthrowing_destructors_(records.nonthrowing_destructors), body_(body) {
    bool has_owner_array = false;
    for (const auto& local : world_.ins.seqs.elements(body.locals)) {
      const auto kind = ownership(local.type);
      flow_.locals.push_back({local.name, kind, (local.flags & sema::local_flag::parameter) != 0, shared_reference(local.type)});
      has_linear_ = has_linear_ || kind == Ownership::linear;
      has_owner_ = has_owner_ || mentions_owner(local.type);
      has_owner_array = has_owner_array || owner_array(local.type);
    }
    for (ast::u32 index = 0; index < world_.ins.seqs.size(body.locals); ++index) {
      const auto local = world_.ins.seqs.at(body.locals, index);
      if (flow_.locals[index].parameter || ast::cv_of(world_.types.canonical(local.type)) != ast::Cv::none) continue;
      const auto* record = ownership_record(local.type);
      if (!record || !record->supported) continue;
      field_slots_.emplace(index, FieldSlot{std::nullopt, field_slots(*record, local.name, "")});
    }
    if (has_owner_array) collect_array_slots(ast::Red(world_.ins.ast, body_.tree));
  }

  Flow build() {
    const auto operation = effect(body_.function);
    if (operation == Effect::verified_observe) {
      if (!has_observer_definition(body_.function))
        flow_.entry = unsupported(0, "verified observation requires a unique supported definition in the checked sources");
      else if (const auto at = unsupported_observation(ast::Red(world_.ins.ast, body_.tree)))
        flow_.entry = unsupported(*at, "verified observation cannot expose or mutate the borrowed resource or call another function");
      return std::move(flow_);
    }
    if (operation != Effect::unknown || resource_lifetime_body()) return {};
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
  struct FieldSlot {
    std::optional<ast::u32> owner;
    std::vector<FieldSlot> children;
    bool array = false;
  };

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

  bool owner_array(QualType type) const {
    const auto array = world_.types.rec(type);
    return array.kind == sema::TypeKind::array && ast::cv_of(world_.types.canonical(array.inner)) == ast::Cv::none &&
           ownership(array.inner) != Ownership::ordinary;
  }

  void collect_array_slots(const ast::Red& node) {
    if (node.is<ast::LocalVar>()) {
      const auto var = node.get<ast::LocalVar>();
      const auto local = world_.ins.seqs.at(body_.locals, var.local);
      // Size comes from the typed initializers, so an unsupported declaration does not
      // allocate a slot for every element merely because its array bound is large.
      if (owner_array(local.type) && !flow_.locals[var.local].parameter && var.init.id != ast::NodeId::none &&
          node.ast().is<ast::InitList>(var.init.id) &&
          (var.destructor == EntityId::none || nonthrowing(var.destructor))) {
        const auto array = world_.types.rec(local.type);
        const auto kind = ownership(array.inner);
        const auto count = node.ast().children(node.ast().get<ast::InitList>(var.init.id).elems).size();
        if (count == array.bound) {
          FieldSlot root{std::nullopt, {}, true};
          root.children.reserve(count);
          for (std::size_t i = 0; i < count; ++i) {
            const auto slot = static_cast<ast::u32>(flow_.locals.size());
            flow_.locals.push_back({local.name, kind, false, false, {}, std::format("[{}]", i)});
            root.children.push_back({slot, {}});
          }
          has_linear_ = has_linear_ || kind == Ownership::linear;
          field_slots_.emplace(var.local, std::move(root));
        }
      }
    }
    node.for_each_child([&](const ast::Red& child) { collect_array_slots(child); });
  }

  const OwnershipRecord* ownership_record(QualType type) const {
    const auto record = world_.types.rec(type);
    if (record.kind != sema::TypeKind::class_) return nullptr;
    return ownership_record(record.entity);
  }

  const OwnershipRecord* ownership_record(EntityId entity) const {
    const auto it = std::ranges::lower_bound(records_, entity, std::ranges::less{}, &OwnershipRecord::entity);
    return it != records_.end() && it->entity == entity ? &*it : nullptr;
  }

  std::vector<FieldSlot> field_slots(const OwnershipRecord& record, ast::NameId local, const std::string& prefix) {
    std::vector<FieldSlot> fields;
    fields.reserve(record.fields.size());
    for (const auto& field : record.fields) {
      const auto name = world_.ins.names.spelling(field.name);
      const auto path = prefix.empty() ? std::string(name) : std::format("{}.{}", prefix, name);
      auto& slot = fields.emplace_back();
      if (field.mode != Ownership::ordinary) {
        slot.owner = static_cast<ast::u32>(flow_.locals.size());
        flow_.locals.push_back({local, field.mode, false, false, {}, path});
        has_linear_ = has_linear_ || field.mode == Ownership::linear;
      } else if (field.record != EntityId::none) {
        slot.children = field_slots(*ownership_record(field.record), local, path);
      }
    }
    return fields;
  }

  bool shared_reference(QualType type) const {
    const auto reference = world_.types.rec(type);
    return reference.kind == sema::TypeKind::reference && !reference.rvalue_ref &&
           ast::cv_of(world_.types.canonical(reference.inner)) == ast::Cv::const_ &&
           ownership(reference.inner) != Ownership::ordinary;
  }

  Effect effect(EntityId function) const {
    return policy_.effect(policy_operation(world_, function, body_.tu.n));
  }

  bool has_observer_definition(EntityId function) const {
    return policy_.has_observer_definition(policy_operation(world_, function, body_.tu.n));
  }

  // The registered resource's construction and cleanup implement the adapter's contract.
  bool resource_lifetime_body() const {
    if (body_.this_type == QualType{}) return false;
    const auto type = world_.types.rec(body_.this_type).inner;
    if (const auto* record = ownership_record(type); record && record->destructor == body_.function) return true;
    const auto* function = world_.entities.find(body_.function);
    return function && (function->kind == sema::EntityKind::constructor || function->kind == sema::EntityKind::destructor) &&
           ownership(type) != Ownership::ordinary;
  }

  bool mentions_owner(QualType type) const {
    if (ownership(type) != Ownership::ordinary || ownership_record(type)) return true;
    const auto t = world_.types.rec(type);
    using enum sema::TypeKind;
    if (t.kind == pointer || t.kind == reference || t.kind == array || t.kind == member_pointer)
      return mentions_owner(t.inner) || (t.kind == member_pointer && mentions_owner(t.member_owner));
    if (t.kind == function)
      return mentions_owner(t.inner) || std::ranges::any_of(t.params, [&](QualType p) { return mentions_owner(p); });
    return false;
  }

  // The red cursor distinguishes the source occurrences of shared AST nodes.
  std::optional<ast::u32> owner_use(const ast::Red& node) const {
    if (node.is<ast::ImplicitConversion>()) {
      const auto cast = node.get<ast::ImplicitConversion>();
      if (cast.conversion == ast::ConversionKind::lvalue_to_rvalue) {
        const auto operand = unparenthesized(at(node, cast.operand));
        if (operand.is<ast::Member>()) {
          const auto member = operand.get<ast::Member>();
          if (const auto* slot = field_slot(operand, member); slot && !slot->owner && slot->children.empty())
            return std::nullopt;  // copy the scalar without exposing its containing record
        }
      }
    }
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

  const FieldSlot* field_slot(const ast::Red& node, const ast::Member& member) const {
    if (member.arrow || member.reference) return nullptr;
    const auto object = unparenthesized(at(node, member.object));
    const FieldSlot* parent = nullptr;
    if (object.is<ast::LocalRef>()) {
      const auto it = field_slots_.find(object.get<ast::LocalRef>().local);
      if (it != field_slots_.end()) parent = &it->second;
    } else if (object.is<ast::Member>()) {
      parent = field_slot(object, object.get<ast::Member>());
    }
    return parent && member.field < parent->children.size() ? &parent->children[member.field] : nullptr;
  }

  std::optional<ast::u32> array_element(const ast::Red& node) const {
    const auto subscript = node.get<ast::Subscript>();
    const auto base = unparenthesized(at(node, subscript.base));
    if (!base.is<ast::ImplicitConversion>()) return std::nullopt;
    const auto decay = base.get<ast::ImplicitConversion>();
    if (decay.conversion != ast::ConversionKind::array_to_pointer) return std::nullopt;
    const auto local = unparenthesized(at(base, decay.operand));
    if (!local.is<ast::LocalRef>()) return std::nullopt;
    const auto root = field_slots_.find(local.get<ast::LocalRef>().local);
    if (root == field_slots_.end() || !root->second.array) return std::nullopt;
    const auto index = unparenthesized(at(node, subscript.index));
    // Read the selected integer literal after sema's promotions. Do not peel a
    // conversion that might have changed which element the expression refers to.
    if (!index.is<ast::Literal>()) return std::nullopt;
    const auto value = index.get<ast::Literal>().value;
    if (!world_.ins.values.is<ast::VInt>(value)) return std::nullopt;
    const auto number = ast::int_bits(world_.ins, value);
    if (number.hi != 0 || number.lo >= root->second.children.size()) return std::nullopt;
    return root->second.children[number.lo].owner;
  }

  bool observation_scalar(QualType type) const {
    const auto canonical = world_.types.canonical(type);
    if ((std::to_underlying(ast::cv_of(canonical)) & std::to_underlying(ast::Cv::volatile_)) != 0) return false;
    return world_.types.is_arithmetic(canonical) || world_.types.rec(canonical).kind == sema::TypeKind::enum_;
  }

  // Assignments can update scalar storage independent of the borrow. Reference variables
  // would let an assignment address the resource indirectly.
  bool observation_storage(const ast::Red& node) const {
    if (node.is<ast::LocalRef>())
      return observation_scalar(world_.ins.seqs.at(body_.locals, node.get<ast::LocalRef>().local).type);
    if (node.is<ast::DeclRef>()) {
      const auto ref = node.get<ast::DeclRef>();
      return !ref.reference && observation_scalar(ref.info.type);
    }
    return false;
  }

  // The only allowed occurrence of the resource parameter is the base of a plain scalar
  // member access. Only non-reference locals and variables may be assignment destinations.
  std::optional<ast::u32> unsupported_observation(const ast::Red& node) const {
    if (node.is<ast::Member>()) {
      const auto member = node.get<ast::Member>();
      const auto object = unparenthesized(at(node, member.object));
      if (!member.arrow && !member.reference && observation_scalar(member.info.type) && object.is<ast::LocalRef>()) {
        const auto slot = object.get<ast::LocalRef>().local;
        if (flow_.locals[slot].parameter && flow_.locals[slot].shared_reference) return std::nullopt;
      }
      return node.offset();
    }
    const bool supported = ast::dispatch<ast::Nodes>(node.kind(), [&]<class T>(std::type_identity<T>) {
      const auto n = node.get<T>();
      if constexpr (std::is_same_v<T, ast::Compound> || std::is_same_v<T, ast::DeclStmt> ||
                    std::is_same_v<T, ast::If> || std::is_same_v<T, ast::Return> ||
                    std::is_same_v<T, ast::ExprStmt> || std::is_same_v<T, ast::Paren>) return true;
      else if constexpr (std::is_same_v<T, ast::LocalRef> || std::is_same_v<T, ast::DeclRef>)
        return observation_storage(node);
      else if constexpr (std::is_same_v<T, ast::LocalVar>)
        return observation_scalar(world_.ins.seqs.at(body_.locals, n.local).type) &&
               n.init.id != ast::NodeId::none && n.destructor == EntityId::none;
      else if constexpr (std::is_same_v<T, ast::Assign>)
        return n.op == ast::AssignOp::assign && observation_storage(unparenthesized(at(node, n.lhs)));
      else if constexpr (std::is_same_v<T, ast::Unary>) {
        using enum ast::UnaryOp;
        return observation_scalar(n.info.type) && (n.op == plus || n.op == minus || n.op == not_ || n.op == compl_);
      } else if constexpr (std::is_same_v<T, ast::ImplicitConversion>) {
        using enum ast::ConversionKind;
        return n.conversion == to_void || (observation_scalar(n.info.type) &&
               (n.conversion == lvalue_to_rvalue || n.conversion == integral || n.conversion == floating ||
                n.conversion == floating_integral || n.conversion == boolean));
      } else if constexpr (std::is_same_v<T, ast::Literal> || std::is_same_v<T, ast::Binary> ||
                           std::is_same_v<T, ast::Conditional> || std::is_same_v<T, ast::ValueInit>)
        return observation_scalar(n.info.type);
      else return false;
    });
    if (!supported) return node.offset();
    std::optional<ast::u32> problem;
    node.for_each_child([&](const ast::Red& child) { if (!problem) problem = unsupported_observation(child); });
    return problem;
  }

  std::optional<ast::u32> named_local(ast::Red node, bool allow_xvalue = true) const {
    for (node = unparenthesized(node); node.is<ast::ImplicitConversion>(); node = unparenthesized(node)) {
      const auto cast = node.get<ast::ImplicitConversion>();
      if (ownership(cast.info.type) == Ownership::ordinary || cast.conversion != ast::ConversionKind::qualification ||
          (!allow_xvalue && cast.info.category != ast::ValueCategory::lvalue))
        return std::nullopt;
      node = at(node, cast.operand);
    }
    if (node.is<ast::LocalRef>()) {
      const auto local = node.get<ast::LocalRef>().local;
      if (flow_.locals[local].ownership != Ownership::ordinary || flow_.locals[local].shared_reference) return local;
    }
    if (node.is<ast::Member>())
      if (const auto* field = field_slot(node, node.get<ast::Member>())) return field->owner;
    if (node.is<ast::Subscript>()) return array_element(node);
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
    if (args.size() == 1) return named_local(at(node, args.front()));
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

  void collect_fields(Scope& scope, const FieldSlot& slot) const {
    if (slot.owner) scope.locals.push_back({*slot.owner, false});
    for (const auto& field : slot.children) collect_fields(scope, field);
  }

  // Declarations directly in this scope. A backedge can reach the same local slot again after
  // its scope exits. Cleanup events for declarations that were skipped see an absent slot.
  void collect(Scope& scope, const ast::Red& node) const {
    if (node.is<ast::LocalVar>()) {
      const auto var = node.get<ast::LocalVar>();
      const bool throws = throwing_destructor(var.destructor);
      const auto& local = flow_.locals[var.local];
      if (local.ownership != Ownership::ordinary || local.shared_reference || throws) scope.locals.push_back({var.local, throws});
      if (const auto it = field_slots_.find(var.local); it != field_slots_.end())
        collect_fields(scope, it->second);
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

  Point initialize_owner(ast::u32 slot, const ast::Red& initializer, Point next) {
    if (initializer.is<ast::Call>()) {
      const auto call = initializer.get<ast::Call>();
      const auto operation = direct_effect(initializer, call);
      if (operation == Effect::issue)
        return add({Event::initialize, initializer.offset(), next, Flow::exit, slot});
      if (operation == Effect::transfer)
        if (const auto source = operation_argument(initializer, call); source && !flow_.locals[*source].shared_reference)
          return add({Event::transfer, initializer.offset(), next, Flow::exit, slot, *source});
    }
    return unsupported(initializer.offset(), "an owner must be initialized by a registered factory or transfer from a named owner");
  }

  Point initialize_fields(const ast::Red& parent, ast::Child child, const FieldSlot& slot, Point next, Context context) {
    if (slot.owner) {
      if (child.id == ast::NodeId::none)
        return unsupported(parent.offset(), "an owner must be initialized by a registered factory or transfer from a named owner");
      return initialize_owner(*slot.owner, unparenthesized(at(parent, child)), next);
    }
    if (slot.children.empty()) return lower_child(parent, child, next, context);
    if (child.id == ast::NodeId::none || !parent.ast().is<ast::InitList>(child.id))
      return unsupported(parent.offset(), "a record with owners requires aggregate initialization");
    const auto init = at(parent, child);
    const auto elements = init.ast().children(init.get<ast::InitList>().elems);
    if (elements.size() != slot.children.size())
      return unsupported(init.offset(), "cannot match aggregate initializers to owned fields");
    for (std::size_t i = elements.size(); i-- > 0;)
      next = initialize_fields(init, elements[i], slot.children[i], next, context);
    return next;
  }

  Point lower_local(const ast::Red& node, Point next, Context context) {
    const auto var = node.get<ast::LocalVar>();
    auto& local = flow_.locals[var.local];
    if (local.shared_reference) {
      if (var.init.id != ast::NodeId::none)
        if (const auto owner = named_local(at(node, var.init), false); owner &&
            flow_.locals[*owner].ownership != Ownership::ordinary) {
          local.borrow = Flow::Borrow{*owner, node.offset()};
          return add({Event::borrow, node.offset(), next, Flow::exit, var.local, *owner});
        }
      return unsupported(node.offset(), "a shared borrow must bind directly to a named owner");
    }
    if (const auto it = field_slots_.find(var.local); it != field_slots_.end())
      return initialize_fields(node, var.init, it->second, next, context);
    if (local.ownership != Ownership::ordinary) {
      if (var.init.id != ast::NodeId::none) return initialize_owner(var.local, unparenthesized(at(node, var.init)), next);
      return unsupported(node.offset(), "an owner must be initialized by a registered factory or transfer from a named owner");
    }
    const auto type = world_.ins.seqs.at(body_.locals, var.local).type;
    if (owner_array(type))
      return unsupported(node.offset(), "an owner array requires aggregate initialization and nonthrowing destruction");
    if (mentions_owner(type))
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
      if (is_observation(operation) || operation == Effect::consume) {
        EntityId observer = EntityId::none;
        if (operation == Effect::verified_observe) {
          const auto key = policy_operation(world_, call.function, body_.tu.n);
          const bool labeled = std::ranges::binary_search(policy_.labeled_observers, key);
          if (labeled || ((!body_.defer_observers || key.second) && !policy_.has_observer_definition(key)))
            return unsupported(node.offset(), "verified observation requires a unique supported definition in the checked sources");
          if (!key.second) observer = call.function;
        }
        if (const auto local = operation_argument(node, call)) {
          if (operation == Effect::consume && flow_.locals[*local].shared_reference)
            return unsupported(node.offset(), "a shared borrow cannot be consumed");
          return add({.event = operation == Effect::consume ? Event::consume : Event::observe,
                      .offset = node.offset(), .next = next, .local = *local, .observer = observer});
        }
        return unsupported(node.offset(), "an ownership protocol operation requires a named owner");
      }
    }
    if (const auto local = named_local(node); local && !flow_.locals[*local].shared_reference)
      return add({Event::observe, node.offset(), next, Flow::exit, *local});
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
    return (entity && nonthrowing_type(entity->type)) || std::ranges::binary_search(nonthrowing_destructors_, function);
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
  std::span<const OwnershipRecord> records_;
  std::span<const EntityId> nonthrowing_destructors_;
  OwnershipBody body_;
  Flow flow_;
  std::map<ast::u32, FieldSlot> field_slots_;
  bool has_owner_ = false;
  bool has_linear_ = false;
};

class OwnershipAnalysis {
 public:
  OwnershipAnalysis(const ast::Interners& in, const Flow& flow) : in_(in), flow_(flow) {
    for (ast::u32 i = 0; i < flow_.locals.size(); ++i)
      if (const auto& borrow = flow_.locals[i].borrow) borrows_[borrow->owner].push_back(i);
  }

  OwnershipBodyCheck run() const {
    if (flow_.entry == Flow::exit) return {};
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
    std::set<EntityId> observers;
    while (!work.empty()) {
      const auto point = work.front();
      work.pop_front();
      const auto& step = flow_.steps[point];
      State state = *paths[point];
      if (auto problem = apply(step, state)) return {std::move(problem), {}};
      if (step.observer != EntityId::none) observers.insert(step.observer);
      propagate(step.next, state);
      if (step.event == Event::branch) propagate(step.other, state);
    }
    return {std::nullopt, {observers.begin(), observers.end()}};
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
    const auto kind = owner.shared_reference ? "shared borrow" :
                      owner.ownership == Ownership::linear ? "linear resource" : "affine resource";
    if (!owner.field.empty())
      return std::format("{} '{}{}{}'", kind, name, owner.field.starts_with('[') ? "" : ".", owner.field);
    return std::format("{} '{}'", kind, name.empty() ? "<unnamed>" : name);
  }

  std::optional<OwnershipFinding> require_live(const Flow::Step& step, ast::u32 local, const Slot& slot) const {
    if (slot.possible == live) return std::nullopt;
    if (slot.spent_at) {
      const auto end = slot.transferred ? " may already have transferred its obligation" : " may already have been consumed";
      return OwnershipFinding{step.offset, description(local) + end, slot.spent_at,
                              slot.transferred ? Event::transfer : Event::consume};
    }
    return OwnershipFinding{step.offset, description(local) + " may be unavailable"};
  }

  std::optional<OwnershipFinding> may_unwind(ast::u32 offset, const State& state) const {
    for (ast::u32 i = 0; i < state.size(); ++i)
      if (flow_.locals[i].ownership == Ownership::linear && (state[i].possible & live))
        return OwnershipFinding{offset, "operation may unwind while " + description(i) + " is live"};
    return std::nullopt;
  }

  std::optional<OwnershipFinding> require_unborrowed(const Flow::Step& step, ast::u32 owner, const State& state) const {
    const auto it = borrows_.find(owner);
    if (it == borrows_.end()) return std::nullopt;
    for (const auto local : it->second)
      if (state[local].possible & live)
        return OwnershipFinding{step.offset, description(owner) + " may still have a live " + description(local),
                                flow_.locals[local].borrow->offset, Event::borrow};
    return std::nullopt;
  }

  std::optional<OwnershipFinding> apply(const Flow::Step& step, State& state) const {
    if (step.event == Event::unsupported) return OwnershipFinding{step.offset, step.problem};
    if (step.event == Event::branch) return std::nullopt;
    if (step.event == Event::may_unwind) return may_unwind(step.offset, state);
    auto& slot = state[step.local];
    switch (step.event) {
    case Event::borrow:
      if (auto problem = require_live(step, step.source, state[step.source])) return problem;
      slot = {live};
      break;
    case Event::observe: return require_live(step, step.local, slot);
    case Event::consume:
      if (auto problem = require_live(step, step.local, slot)) return problem;
      if (auto problem = require_unborrowed(step, step.local, state)) return problem;
      slot = {spent, step.offset};
      break;
    case Event::transfer:
      if (auto problem = require_live(step, step.source, state[step.source])) return problem;
      if (auto problem = require_unborrowed(step, step.source, state)) return problem;
      state[step.source] = {spent, step.offset, true};
      [[fallthrough]];
    case Event::initialize:
      if (flow_.locals[step.local].ownership == Ownership::linear && (slot.possible & live))
        return OwnershipFinding{step.offset, "initialization would overwrite live " + description(step.local)};
      slot = {live};
      break;
    case Event::leave:
      if (auto problem = require_unborrowed(step, step.local, state)) return problem;
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
  std::map<ast::u32, std::vector<ast::u32>> borrows_;
};

}  // namespace

struct PolicyDeclarationsFor {
  using Key = ast::u32;
  using Value = PolicyDeclarations;
  static engine::query_task<Value> run(engine::Ctx& cx, Key unit) {
    const TuId tu{unit};
    const auto& front = co_await cx.fetch<TuFront>(tu);
    if (front.policy_aliases.empty() && front.policy_annotations.empty()) co_return Value{};
    const auto& source = cx.fetch<pp::SourceOf>(pp::FileId{unit});
    co_return read_policy_declarations(*universe().world_for(*source.config.target), unit, front.policy_aliases,
                                     front.policy_annotations);
  }
};

struct PolicyRegistryFor {
  using Key = PolicySources;
  using Value = PolicyRegistry;
  static engine::query_task<Value> run(engine::Ctx& cx, Key sources) {
    const auto units = universe().in->seqs.elements(sources);
    if (units.empty()) co_return Value{};
    const auto fragments = co_await cx.par_map<PolicyDeclarationsFor>(units);
    const auto& source = cx.fetch<pp::SourceOf>(pp::FileId{units.front()});
    const auto& world = *universe().world_for(*source.config.target);
    auto registry = compose_policy_registry(world, fragments);
    const bool verified_observations = std::ranges::any_of(registry.ownership.operations, [](const auto& operation) {
      return operation.second == Effect::verified_observe;
    });
    if (!registry.problems.empty() || (registry.effects.empty() && !verified_observations)) co_return registry;

    const auto fronts = co_await cx.par_map<TuFront>(units | std::views::transform([](ast::u32 unit) { return TuId{unit}; }));
    std::map<PolicyOperation, bool> observers, checked_effects;
    std::set<PolicyOperation> labeled_observers, labeled_effects;
    const auto add_definition = [](auto& definitions, PolicyOperation operation, bool supported) {
      const auto [it, fresh] = definitions.try_emplace(operation, supported);
      if (!fresh) it->second = false;
    };
    for (std::size_t i = 0; i < fronts.size(); ++i) {
      const auto& unit = fronts[i].get().unit;
      for (const auto& entity : unit.entities) {
        if (entity.asm_label.empty()) continue;
        const auto operation = policy_operation(world, entity.id, units[i]);
        if (registry.ownership.effect(operation) == Effect::verified_observe) labeled_observers.insert(operation);
        if (registry.effects.contains(operation)) labeled_effects.insert(operation);
      }
      for (const auto& def : unit.functions) {
        const auto operation = policy_operation(world, def.entity, units[i]);
        const bool strong = !def.coalesced && !(def.decl.flags & (sema::decl_flag::inline_ | sema::decl_flag::constexpr_));
        const bool supported = def.body.tree != ast::NodeId::none && def.decl.contracts == ast::NodeId::none &&
                               (operation.second || strong);
        if (registry.ownership.effect(operation) == Effect::verified_observe)
          add_definition(observers, operation, supported);
        if (const auto effect = registry.effects.find(operation); effect != registry.effects.end() && !effect->second.trusted)
          add_definition(checked_effects, operation, supported && !(def.decl.flags & sema::decl_flag::consteval_));
      }
    }
    for (const auto& [operation, supported] : observers)
      if (supported && !labeled_observers.contains(operation)) registry.ownership.observer_definitions.push_back(operation);
    registry.ownership.labeled_observers.assign(labeled_observers.begin(), labeled_observers.end());
    for (const auto& [operation, bound] : registry.effects) {
      const auto origin = registry.effect_origins.at(operation);
      if (labeled_effects.contains(operation)) {
        registry.problems[origin.unit].push_back({origin, "effect bound does not support functions with an assembly label"});
      } else if (!bound.trusted) {
        const auto def = checked_effects.find(operation);
        if (def == checked_effects.end() && !operation.second) {
          registry.external_effect_declarations.insert(operation);
        } else if (def == checked_effects.end() || !def->second) {
          registry.problems[origin.unit].push_back({origin, "verified effect bound requires a unique supported definition in the checked source inputs"});
        }
      }
    }
    co_return registry;
  }
};

struct OwnershipRecordsFor {
  struct Key {
    PolicySources sources;
    TuId tu;
    bool operator==(const Key&) const = default;
  };
  using Value = OwnershipRecords;
  static engine::query_task<Value> run(engine::Ctx& cx, Key key) {
    const auto& registry = co_await cx.fetch<PolicyRegistryFor>(key.sources);
    if (registry.ownership.resources.empty()) co_return Value{};
    const auto& shapes = co_await cx.fetch<UnitShapes>(key.tu);
    const auto& source = cx.fetch<pp::SourceOf>(pp::FileId{key.tu.n});
    const auto& types = universe().world_for(*source.config.target)->types;

    // Trace containment in reverse from registered owners. Affected types outside the
    // supported value containment remain visible so the checker can reject their use.
    std::map<EntityId, std::vector<EntityId>> enclosing;
    std::map<EntityId, const ClassShape*> by_entity;
    Value result;
    for (const auto& shape : shapes.classes) {
      by_entity.emplace(shape.entity, &shape);
      if (shape.nonthrowing_destructor != EntityId::none)
        result.nonthrowing_destructors.push_back(shape.nonthrowing_destructor);
      for (const auto base : shape.bases) enclosing[base].push_back(shape.entity);
      std::set<QualType> seen;
      auto collect = [&](auto&& self, QualType type) -> void {
        if (!seen.insert(type).second) return;
        const auto t = types.rec(type);
        using enum sema::TypeKind;
        switch (t.kind) {
          case class_: enclosing[t.entity].push_back(shape.entity); break;
          case member_pointer: self(self, t.member_owner); [[fallthrough]];
          case pointer: case reference: case array: self(self, t.inner); break;
          case function:
            self(self, t.inner);
            for (const auto param : t.params) self(self, param);
            break;
          default: break;
        }
      };
      for (const auto field : shape.fields) collect(collect, field);
    }
    std::set<EntityId> affected;
    std::vector<EntityId> pending;
    for (const auto& [type, mode] : registry.ownership.resources) {
      const auto id = types.class_of(type);
      if (affected.insert(id).second) pending.push_back(id);
    }
    while (!pending.empty()) {
      const auto owner = pending.back();
      pending.pop_back();
      const auto parents = enclosing.find(owner);
      if (parents != enclosing.end())
        for (const auto parent : parents->second)
          if (affected.insert(parent).second) pending.push_back(parent);
    }
    for (const auto& [type, mode] : registry.ownership.resources) affected.erase(types.class_of(type));

    std::ranges::sort(result.nonthrowing_destructors);
    auto& records = result.records;
    for (const auto id : affected) {
      const auto it = by_entity.find(id);
      if (it == by_entity.end()) continue;
      const auto& shape = *it->second;
      OwnershipRecord record{id, {}, shape.plain_ownership_record, shape.ownership_destructor};
      for (std::size_t i = 0; i < shape.fields.size(); ++i) {
        const auto type = types.canonical(shape.fields[i]);
        const auto mode = registry.ownership.ownership(type);
        const auto field = types.rec(type);
        auto child = EntityId::none;
        if (mode != Ownership::ordinary) {
          record.supported = record.supported && ast::cv_of(type) == ast::Cv::none;
        } else if (field.kind == sema::TypeKind::class_ && affected.contains(field.entity)) {
          child = field.entity;
          record.supported = record.supported && ast::cv_of(type) == ast::Cv::none;
        } else {
          record.supported = record.supported && (types.is_arithmetic(type) || field.kind == sema::TypeKind::enum_);
        }
        record.fields.push_back({shape.field_names[i], mode, child});
      }
      records.push_back(std::move(record));
    }
    std::map<EntityId, bool> supported;
    const auto check = [&](auto&& self, const OwnershipRecord& record) -> bool {
      if (!record.supported) return false;
      const auto [cached, inserted] = supported.try_emplace(record.entity, false);
      if (!inserted) return cached->second;
      for (const auto& field : record.fields) {
        if (field.record == EntityId::none) continue;
        const auto child = std::ranges::lower_bound(records, field.record, std::ranges::less{}, &OwnershipRecord::entity);
        if (child == records.end() || child->entity != field.record || !self(self, *child)) return false;
      }
      return cached->second = true;
    };
    for (auto& record : records) {
      if (check(check, record)) continue;
      record.supported = false;
      record.fields.clear();
      record.destructor = EntityId::none;
    }
    co_return result;
  }
};

struct BuildOwnershipFlow {
  using Key = OwnershipBody;
  using Value = OwnershipFlow;
  static engine::query_task<Value> run(engine::Ctx& cx, Key body) {
    const auto& registry = co_await cx.fetch<PolicyRegistryFor>(body.sources);
    const auto& records = co_await cx.fetch<OwnershipRecordsFor>({body.sources, body.tu});
    const auto& target = body.linux_target ? ast::x86_64_linux : ast::aarch64_darwin;
    co_return OwnershipFlowBuilder(*universe().world_for(target), registry.ownership, records, body).build();
  }
};

struct CheckOwnershipBody {
  using Key = OwnershipBody;
  using Value = OwnershipBodyCheck;
  static engine::query_task<Value> run(engine::Ctx& cx, Key body) {
    const auto& flow = co_await cx.fetch<BuildOwnershipFlow>(body);
    co_return OwnershipAnalysis(*universe().in, flow).run();
  }
};

struct EffectBody {
  ast::NodeId tree;
  ast::SeqId<ast::LocalRec> locals;
  bool linux_target;
  bool operator==(const EffectBody&) const = default;
};

struct EffectFinding {
  ast::u32 offset;
  std::string message;
};

struct EffectCalls {
  std::vector<std::pair<EntityId, ast::u32>> calls;
  std::optional<EffectFinding> unsupported;
  std::optional<ast::u32> grade_jump;
};

struct EffectBodyCheck {
  std::optional<EffectFinding> finding;
  std::vector<EntityId> callees;
};

// A bound includes every possible call, regardless of control flow. Only AST nodes whose
// evaluation needs no unmodeled calls are allowed. Constructors, class copies, destruction,
// allocation, exceptions and virtual dispatch must be modeled before adding their nodes.
struct EffectCallsFor {
  using Key = EffectBody;
  using Value = EffectCalls;
  static engine::query_task<Value> run(engine::Ctx&, Key body) {
    const auto& world = *universe().world_for(body.linux_target ? ast::x86_64_linux : ast::aarch64_darwin);
    const auto locals = world.ins.seqs.elements(body.locals);
    Value result;
    std::set<EntityId> seen;
    auto visit = [&](auto&& self, const ast::Red& node) -> void {
      if (result.unsupported || node.is<ast::StaticAssert>()) return;
      if (!result.grade_jump && (node.is<ast::Goto>() || node.is<ast::Switch>()))
        result.grade_jump = node.offset();
      const auto problem = ast::dispatch<ast::Nodes>(node.kind(), [&]<class T>(std::type_identity<T>) -> std::string_view {
        using namespace ast;
        if constexpr (is_expression(^^T)) {
          if (sema::Types::is_volatile(node.get<T>().info.type)) return "volatile access is outside the checked effect subset";
        }
        if constexpr (std::is_same_v<T, Call>) {
          const auto target = node.get<T>().function;
          if (target == EntityId::none) return "indirect call has no verified effect bound";
          if (seen.insert(target).second) result.calls.emplace_back(target, node.offset());
        } else if constexpr (std::is_same_v<T, LocalVar>) {
          const auto local = node.get<T>();
          const auto type = locals[local.local].type;
          if (local.destructor != EntityId::none || !(world.types.is_scalar(type) || world.types.is_reference(type)))
            return "local object initialization or cleanup is outside the checked effect subset";
        } else if constexpr (std::is_same_v<T, ImplicitConversion>) {
          const auto conversion = node.get<T>().conversion;
          if (conversion == ConversionKind::user_defined || conversion == ConversionKind::array_copy)
            return "implicit conversion is outside the checked effect subset";
        } else if constexpr (std::is_same_v<T, Assign>) {
          if (node.get<T>().op == AssignOp::trivial_class_copy)
            return "class assignment is outside the checked effect subset";
        } else if constexpr (!std::disjunction_v<
                                 std::is_same<T, Literal>, std::is_same<T, DeclRef>, std::is_same<T, LocalRef>,
                                 std::is_same<T, Paren>, std::is_same<T, Unary>, std::is_same<T, Binary>,
                                 std::is_same<T, OrderedBinary>, std::is_same<T, Conditional>, std::is_same<T, Subscript>,
                                 std::is_same<T, Member>, std::is_same<T, ValueInit>, std::is_same<T, DefaultInit>,
                                 std::is_same<T, DefaultArg>, std::is_same<T, ExprStmt>, std::is_same<T, Return>,
                                 std::is_same<T, Compound>, std::is_same<T, If>, std::is_same<T, While>,
                                 std::is_same<T, Do>, std::is_same<T, For>, std::is_same<T, Break>, std::is_same<T, Continue>,
                                 std::is_same<T, DeclStmt>, std::is_same<T, CondDecl>, std::is_same<T, Switch>,
                                 std::is_same<T, Case>, std::is_same<T, Default>, std::is_same<T, Label>, std::is_same<T, Goto>>) {
          return "operation is outside the checked effect subset";
        }
        return {};
      });
      if (!problem.empty()) {
        result.unsupported = EffectFinding{node.offset(), std::string(problem)};
        return;
      }
      node.for_each_child([&](const ast::Red& child) { self(self, child); });
    };
    visit(visit, ast::Red(world.ins.ast, body.tree));
    co_return result;
  }
};

struct EffectBoundFor {
  struct Key {
    PolicySources sources;
    TuId tu;
    EntityId function;
    bool operator==(const Key&) const = default;
  };
  using Value = std::optional<EffectBound>;
  static engine::query_task<Value> run(engine::Ctx& cx, Key key) {
    const auto& registry = co_await cx.fetch<PolicyRegistryFor>(key.sources);
    const auto& source = cx.fetch<pp::SourceOf>(pp::FileId{key.tu.n});
    const auto& world = *universe().world_for(*source.config.target);
    const auto bound = registry.effects.find(policy_operation(world, key.function, key.tu.n));
    if (bound == registry.effects.end()) co_return std::nullopt;
    co_return bound->second;
  }
};

struct EffectCost {
  bool unbounded = false;
  ast::u64 uses = 0;
  auto operator<=>(const EffectCost&) const = default;
};

struct EffectPaths {
  std::optional<EffectCost> continues = EffectCost{};
  std::optional<EffectCost> returns;
};

static EffectPaths sequence(EffectPaths first, EffectPaths second) {
  const auto add = [](std::optional<EffectCost> a, std::optional<EffectCost> b) -> std::optional<EffectCost> {
    if (!a || !b) return std::nullopt;
    if (a->unbounded || b->unbounded || a->uses > std::numeric_limits<ast::u64>::max() - b->uses)
      return EffectCost{true};
    return EffectCost{false, a->uses + b->uses};
  };
  return {add(first.continues, second.continues),
          std::max(first.returns, add(first.continues, second.returns))};
}

static EffectPaths choice(EffectPaths first, EffectPaths second) {
  return {std::max(first.continues, second.continues), std::max(first.returns, second.returns)};
}

static EffectCost maximum(EffectPaths paths) {
  return std::max(paths.continues, paths.returns).value_or(EffectCost{});
}

// Count one tag over the structured AST. A return closes its path. A loop with positive
// cost in its repeating region widens to unbounded. An initializer in a for loop runs once.
static EffectPaths effect_usage(const ast::Red& node, const std::map<EntityId, ast::u64>& costs) {
  const auto child = [&](ast::Child edge) -> EffectPaths {
    if (edge.id == ast::NodeId::none) return {};
    return effect_usage(ast::Red(node.ast(), edge.id, node.offset() + edge.at), costs);
  };
  if (node.is<ast::StaticAssert>()) return {};
  if (node.is<ast::If>()) {
    const auto branch = node.get<ast::If>();
    return sequence(sequence(child(branch.init), child(branch.condition)),
                    choice(child(branch.then_), child(branch.else_)));
  }
  if (node.is<ast::Conditional>()) {
    const auto branch = node.get<ast::Conditional>();
    return sequence(child(branch.condition), choice(child(branch.then_), child(branch.else_)));
  }
  if (node.is<ast::For>()) {
    const auto loop = node.get<ast::For>();
    const auto iteration = sequence(child(loop.condition), sequence(child(loop.body), child(loop.increment)));
    return sequence(child(loop.init), {EffectCost{maximum(iteration) > EffectCost{}}, {}});
  }

  EffectPaths paths;
  node.for_each_child([&](const ast::Red& edge) { paths = sequence(paths, effect_usage(edge, costs)); });
  if (node.is<ast::While>() || node.is<ast::Do>())
    return {EffectCost{maximum(paths) > EffectCost{}}, {}};
  if (node.is<ast::Return>()) return {std::nullopt, std::max(paths.continues, paths.returns)};
  if (node.is<ast::Call>())
    if (const auto found = costs.find(node.get<ast::Call>().function); found != costs.end())
      return sequence(paths, {EffectCost{false, found->second}, {}});
  return paths;
}

static std::optional<EffectFinding> check_usage_grades(const sema::World& world, const ast::Red& body,
                                                       const EffectCalls& calls, const EffectBound& caller,
                                                       const std::map<EntityId, const EffectBound*>& bounds) {
  for (const auto& [tag, limit] : caller.grades) {
    std::map<EntityId, ast::u64> costs;
    for (const auto& [target, offset] : calls.calls) {
      const auto& callee = *bounds.at(target);
      if (!std::ranges::binary_search(callee.tags, tag)) continue;
      const auto grade = std::ranges::lower_bound(callee.grades, tag, {}, &decltype(callee.grades)::value_type::first);
      if (grade == callee.grades.end() || grade->first != tag) {
        const auto name = world.ins.names.spelling(world.entities.rec(target).name);
        return EffectFinding{offset, std::format("call to '{}' has no finite usage grade for effect '{}'",
                                                 name, world.types.spell(tag))};
      }
      costs.emplace(target, grade->second);
    }
    if (costs.empty()) continue;
    if (calls.grade_jump)
      return EffectFinding{*calls.grade_jump, "a finite usage grade with effectful calls requires control flow without goto or switch"};
    const auto use = maximum(effect_usage(body, costs));
    if (use.unbounded || use.uses > limit)
      return EffectFinding{body.offset(), std::format("effect '{}' exceeds its usage grade of {}{}",
                                                     world.types.spell(tag), limit,
                                                     use.unbounded ? " (unbounded usage)" : "")};
  }
  return std::nullopt;
}

struct CheckEffectBody {
  struct Key {
    PolicySources sources;
    TuId tu;
    EntityId function;
    EffectBody body;
    bool operator==(const Key&) const = default;
  };
  using Value = EffectBodyCheck;
  static engine::query_task<Value> run(engine::Ctx& cx, Key key) {
    const auto& calls = co_await cx.fetch<EffectCallsFor>(key.body);
    if (calls.unsupported) co_return Value{calls.unsupported, {}};
    const auto& world = *universe().world_for(key.body.linux_target ? ast::x86_64_linux : ast::aarch64_darwin);
    const auto& caller = co_await cx.fetch<EffectBoundFor>({key.sources, key.tu, key.function});
    if (!caller || caller->trusted) co_return Value{};
    Value out;
    std::map<EntityId, const EffectBound*> bounds;
    for (const auto& [target, offset] : calls.calls) {
      const auto& callee = co_await cx.fetch<EffectBoundFor>({key.sources, key.tu, target});
      const auto name = world.ins.names.spelling(world.entities.rec(target).name);
      if (!callee)
        co_return Value{EffectFinding{offset, std::format("call to '{}' has no declared effect bound", name)}, {}};
      const auto missing = std::ranges::find_if(callee->tags, [&](QualType tag) {
        return !std::ranges::binary_search(caller->tags, tag);
      });
      if (missing != callee->tags.end())
        co_return Value{EffectFinding{offset, std::format("effect bound of '{}' exceeds the caller's declared bound (effect '{}')",
                                                         name, world.types.spell(*missing))}, {}};
      if (!caller->grades.empty()) bounds.emplace(target, &*callee);
      if (!policy_operation(world, target, key.tu.n).second) out.callees.push_back(target);
    }
    if (!caller->grades.empty())
      out.finding = check_usage_grades(world, ast::Red(world.ins.ast, key.body.tree), calls, *caller, bounds);
    co_return out;
  }
};

enum class EffectVerification { complete_sources, deferred_objects, unavailable };

struct PolicyCheckUnit {
  PolicySources sources;
  TuId tu;
  bool defer_observers;
  EffectVerification effect_verification;
  bool operator==(const PolicyCheckUnit&) const = default;
};

struct CheckedPolicies {
  std::vector<Message> messages;
  std::vector<PolicyContract> contracts;
};

struct CheckPolicies {
  using Key = PolicyCheckUnit;
  using Value = CheckedPolicies;
  static engine::query_task<Value> run(engine::Ctx&, Key);
};

engine::query_task<CheckPolicies::Value> CheckPolicies::run(engine::Ctx& cx, Key key) {
  const TuId tu = key.tu;
  const FrontResult& front = co_await cx.fetch<TuFront>(tu);
  if (front.has_errors()) co_return Value{};
  const auto& registry = co_await cx.fetch<PolicyRegistryFor>(key.sources);
  if (registry.ownership.resources.empty() && registry.effects.empty() && registry.problems.empty()) co_return Value{};
  if (key.effect_verification == EffectVerification::unavailable && !registry.effects.empty() && registry.problems.empty())
    co_return Value{{{std::nullopt, true, "checked effect bounds currently require explicit objects or sources at link", {}}}, {}};
  if (front.unit.virtual_emission_blocked && !registry.ownership.resources.empty() && registry.problems.empty())
    co_return Value{{{std::nullopt, true, "ownership analysis is unavailable for this unit's virtual declarations", {}}}, {}};
  std::vector<PolicyProblem> problems;
  if (const auto it = registry.problems.find(tu.n); it != registry.problems.end()) problems = it->second;
  const bool missing_effects = key.effect_verification == EffectVerification::complete_sources &&
                               !registry.external_effect_declarations.empty();
  if (missing_effects)
    for (const auto& operation : registry.external_effect_declarations) {
      const auto origin = registry.effect_origins.at(operation);
      if (origin.unit == tu.n)
        problems.push_back({origin, "verified effect bound requires a unique supported definition in the checked source inputs"});
    }

  const pp::FileId file{tu.n};
  const pp::Source& source = cx.fetch<pp::SourceOf>(file);
  const auto& world = *universe().world_for(*source.config.target);
  const bool linux_target = source.config.target->triple == ast::x86_64_linux.triple;
  std::vector<const FunctionDef*> definitions;
  std::vector<OwnershipBody> bodies;
  std::vector<const FunctionDef*> effect_definitions;
  std::vector<CheckEffectBody::Key> effect_bodies;
  if (registry.problems.empty() && !missing_effects)
    for (const auto& def : front.unit.functions) {
      if (def.body.tree == ast::NodeId::none) continue;
      if (!registry.ownership.resources.empty()) {
        definitions.push_back(&def);
        bodies.push_back({key.sources, tu, def.entity, def.decl.type, def.body.tree, def.body.locals,
                          def.this_type.value_or(QualType{}), linux_target, key.defer_observers});
      }
      const auto effect = registry.effects.find(policy_operation(world, def.entity, tu.n));
      if (effect != registry.effects.end() && !effect->second.trusted) {
        effect_definitions.push_back(&def);
        effect_bodies.push_back({key.sources, tu, def.entity, {def.body.tree, def.body.locals, linux_target}});
      }
    }
  const auto findings = co_await cx.par_map<CheckOwnershipBody>(bodies);
  const auto effect_findings = co_await cx.par_map<CheckEffectBody>(effect_bodies);
  Value result;
  if (problems.empty() &&
      std::ranges::none_of(findings, [](const auto& finding) { return finding.get().finding.has_value(); }) &&
      std::ranges::none_of(effect_findings, [](const auto& finding) { return finding.get().finding.has_value(); })) {
    if (key.defer_observers) {
      auto signature = [&](EntityId observer) {
        const auto type = world.types.canonical(world.entities.rec(observer).type);
        const auto reference = world.types.rec(world.types.rec(type).params.front());
        const auto mode = registry.ownership.ownership(world.types.canonical(reference.inner));
        return fingerprint(std::tuple{observer, type, mode});
      };
      for (std::size_t i = 0; i < findings.size(); ++i) {
        const auto function = definitions[i]->entity;
        const auto operation = policy_operation(world, function, tu.n);
        if (!operation.second && registry.ownership.effect(operation) == Effect::verified_observe)
          result.contracts.push_back({function, function, signature(function), PolicyContractKind::observation, true});
        for (const auto observer : findings[i].get().observers)
          result.contracts.push_back({function, observer, signature(observer), PolicyContractKind::observation, false});
      }
      auto effect_signature = [&](EntityId function, const EffectBound& bound) {
        // Handles have process-local order. The digest of an exported set uses stable type IDs.
        auto tags = bound.tags | std::views::transform([&](QualType tag) { return world.types.fp(tag); })
                    | std::ranges::to<std::vector>();
        std::ranges::sort(tags);
        const auto type = world.types.canonical(world.entities.rec(function).type);
        const auto base = fingerprint(std::tuple{function, type, std::move(tags)});
        if (bound.grades.empty()) return base;
        auto grades = bound.grades | std::views::transform([&](const auto& grade) {
          return std::pair{world.types.fp(grade.first), grade.second};
        }) | std::ranges::to<std::vector>();
        std::ranges::sort(grades);
        return fingerprint(std::tuple{base, std::move(grades)});
      };
      for (std::size_t i = 0; i < effect_findings.size(); ++i) {
        const auto function = effect_definitions[i]->entity;
        const auto operation = policy_operation(world, function, tu.n);
        const auto& bound = registry.effects.at(operation);
        if (!operation.second)
          result.contracts.push_back({function, function, effect_signature(function, bound), PolicyContractKind::effect, true});
        for (const auto callee : effect_findings[i].get().callees) {
          const auto& declared = registry.effects.at(policy_operation(world, callee, tu.n));
          const auto kind = declared.trusted ? PolicyContractKind::trusted_effect : PolicyContractKind::effect;
          result.contracts.push_back({function, callee, effect_signature(callee, declared), kind, false});
        }
      }
    }
    co_return result;
  }

  const pp::Region& region = co_await cx.fetch<pp::Pp>(file);
  const items::Items& items = co_await cx.fetch<items::RegionItems>(file);
  SourceLocations locations(cx, source, file, region, items);
  auto& in = *universe().in;
  auto& messages = result.messages;
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
    const auto& found = findings[i].get().finding;
    if (!found) continue;
    const auto& finding = *found;
    const auto& def = *definitions[i];
    const auto at = locations.at(in, {def.item, def.body_offset + finding.offset});
    std::string text = finding.message;
    if (finding.previous)
      if (const auto previous = locations.at(in, {def.item, def.body_offset + *finding.previous})) {
        std::string_view event = "consumption";
        if (finding.previous_event == Event::transfer) event = "transfer";
        if (finding.previous_event == Event::borrow) event = "borrow";
        text += std::format(" (previous {} at {})", event, previous->where);
      }
    messages.push_back({at ? std::optional(at->offset) : std::nullopt, true, std::move(text), at ? at->where : ""});
  }
  for (std::size_t i = 0; i < effect_findings.size(); ++i) {
    const auto& finding = effect_findings[i].get().finding;
    if (!finding) continue;
    const auto& def = *effect_definitions[i];
    const auto at = locations.at(in, {def.item, def.body_offset + finding->offset});
    messages.push_back({at ? std::optional(at->offset) : std::nullopt, true, finding->message, at ? at->where : ""});
  }
  co_return result;
}

}  // namespace ycxx::pipeline
