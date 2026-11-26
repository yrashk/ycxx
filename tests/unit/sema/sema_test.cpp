// The parser and sema (#41): the trees, records and diagnostics the per-item queries produce for
// the α subset, as function_ir (#24) will read them: implicit conversions explicit, value
// categories, parameters as the first locals, entities shared by redeclarations, constant
// initializers folded, and positions relative to the parent (ast.md §3.3–§3.6).
import std;
import ycxx.ast;
import ycxx.engine;
import ycxx.pp;
import ycxx.items;
import ycxx.lex;
import ycxx.sema;
import ycxx.parse;
import ycxx.unit;

using namespace ycxx;
using ast::ConversionKind;
using ast::NodeId;
using sema::Builtin;
using unit::check;

struct Analysed {
  pp::Preprocessed pre;
  items::Items its;
  ast::Interners ins;
  sema::World w{ins};
  std::optional<parse::Program> p;

  explicit Analysed(std::string_view text) : pre(pp::preprocess(text)), its(items::split(pre.region, {})) {
    p.emplace(w, pre.region, its);
    p->analyze_all();
  }
  const ast::ItemSema& item(ast::u32 k) { return p->item_sema(k).value; }
  ast::DeclSema decl(ast::u32 k, ast::u32 ord = 0) { return ins.seqs.at(item(k).decls, ord); }
  const ast::BodySema& body(ast::u32 k) { return p->analyze_body(k).value; }
  std::vector<engine::Diagnostic> diags() { return p->diagnostics(); }
  template <class T>
  T get(NodeId n) { return ins.ast.get<T>(n); }
  template <class T>
  bool is(NodeId n) { return ins.ast.is<T>(n); }
  std::string spell(ast::QualType t) { return w.types.spell(t); }
  std::vector<ast::Child> children(ast::ChildList l) {
    auto s = ins.ast.children(l);
    return {s.begin(), s.end()};
  }
};

void prototype_parameters_are_not_body_locals() {
  Analysed a("struct Token {}; int f(int value) { "
             "using Plain = int (*)(Token prototype_only) noexcept; "
             "using Checked = int (*)(Token prototype_only) noexcept(sizeof(prototype_only) == sizeof(Token)); "
             "return value; }");
  check(a.diags().empty(), "both local function-prototype aliases are valid");
  const auto locals = a.body(1).locals;
  check(a.ins.seqs.size(locals) == 1 && a.ins.names.spelling(a.ins.seqs.at(locals, 0).name) == "value",
        "only the actual parameter belongs to the enclosing body");
}

void conversions_and_locals() {
  Analysed a("int f(int a, long b) { return a + b; }");
  check(a.diags().empty(), "no diagnostics");
  ast::DeclSema f = a.decl(0);
  check(f.kind == ast::DeclKind::function && a.spell(f.type) == "int (int, long)", "f's record");
  check(a.ins.seqs.size(f.params) == 2 && a.ins.names.spelling(a.ins.seqs.at(f.params, 1).name) == "b", "params");
  const ast::BodySema& b = a.body(0);
  check(a.ins.seqs.size(b.locals) == 2 && (a.ins.seqs.at(b.locals, 0).flags & sema::local_flag::parameter),
        "the parameters are the first locals");
  auto block = a.get<ast::Compound>(b.tree);
  auto stmts = a.children(block.stmts);
  check(stmts.size() == 1 && stmts[0].at == 1, "one statement, after the '{'");
  auto ret = a.get<ast::Return>(stmts[0].id);
  // return a + b; : the sum is computed in long, then converted to int.
  auto to_int = a.get<ast::ImplicitConversion>(ret.value.id);
  check(to_int.conversion == ConversionKind::integral && a.w.types.is(to_int.info.type, Builtin::int_),
        "long -> int for the return");
  check(ret.value.at == 1, "the operand starts after 'return'");
  auto sum = a.get<ast::Binary>(to_int.operand.id);
  check(sum.op == ast::BinaryOp::add && a.w.types.is(sum.info.type, Builtin::long_) &&
            sum.info.category == ast::ValueCategory::prvalue,
        "the sum is a long prvalue");
  check(sum.lhs.at == 0 && sum.rhs.at == 2, "operands at their offsets in the sum");
  auto widen = a.get<ast::ImplicitConversion>(sum.lhs.id);
  check(widen.conversion == ConversionKind::integral && a.w.types.is(widen.info.type, Builtin::long_), "a widened");
  auto load = a.get<ast::ImplicitConversion>(widen.operand.id);
  check(load.conversion == ConversionKind::lvalue_to_rvalue, "a loaded");
  auto ref = a.get<ast::LocalRef>(load.operand.id);
  check(ref.local == 0 && ref.info.category == ast::ValueCategory::lvalue, "a is local 0, an lvalue");
  auto rload = a.get<ast::ImplicitConversion>(sum.rhs.id);
  check(rload.conversion == ConversionKind::lvalue_to_rvalue && a.get<ast::LocalRef>(rload.operand.id).local == 1,
        "b loaded, no conversion");
}

void redeclarations_and_linkage() {
  Analysed a(
      "extern \"C\" int puts(const char*);\n"
      "int g(int);\n"
      "int g(int x) { return puts(\"hi\") + x; }\n"
      "extern \"C\" { int putchar(int); }\n");
  check(a.diags().empty(), "no diagnostics");
  ast::DeclSema puts = a.decl(0);
  check((puts.flags & sema::decl_flag::lang_c) && a.w.entities.rec(puts.entity).lang == sema::LangLinkage::c,
        "puts has C language linkage");
  check(a.spell(puts.type) == "int (const char *)", "puts's type");
  check(a.decl(1).entity == a.decl(2).entity, "a redeclaration names the same entity");
  check(!(a.decl(1).flags & sema::decl_flag::definition) && (a.decl(2).flags & sema::decl_flag::definition),
        "only the second is a definition");
  // The linkage block's member is its own item, with the block's language linkage.
  ast::u32 member = static_cast<ast::u32>(a.its.items.size() - 1);
  check(a.w.entities.rec(a.decl(member).entity).lang == sema::LangLinkage::c, "putchar inside extern \"C\" { }");
  // The call: callee named by entity, argument converted from the array.
  auto ret = a.get<ast::Return>(a.children(a.get<ast::Compound>(a.body(2).tree).stmts)[0].id);
  auto sum = a.get<ast::Binary>(ret.value.id);
  auto call = a.get<ast::Call>(sum.lhs.id);
  check(call.function == puts.entity && a.is<ast::DeclRef>(call.callee.id), "puts called by entity");
  auto arg = a.get<ast::ImplicitConversion>(a.children(call.args)[0].id);
  check(arg.conversion == ConversionKind::array_to_pointer && a.spell(arg.info.type) == "const char *",
        "the string decays to const char*");
  auto lit = a.get<ast::Literal>(arg.operand.id);
  const sema::ValueRec& v = a.w.values.rec(lit.value);
  check(v.kind == sema::ValueKind::string && v.units == std::vector<std::uint32_t>{'h', 'i', 0}, "the string's units");
  check(lit.info.category == ast::ValueCategory::lvalue && a.spell(lit.info.type) == "const char[3]",
        "a string literal is an lvalue array");
}

void declaration_annotations() {
  Analysed a(R"cpp(
enum class Role { linear = 2, consumes = 5 };
enum class Other { consumes = 5 };
struct Tag { int value; };
consteval int code(int x) { return x + 1; }
struct [[= Role::linear]] Owner;
struct [[deprecated]][[= Role::linear, = Tag{code(6)}]] Owner { int packed; };
[[deprecated]][[= Role::consumes, = Role::consumes]] int finish(Owner&&);
[[= Other::consumes]] int finish(int);
int finish [[=code(6)]] (Owner&&) { return 0; }
int invoke() { return finish(Owner{}); }
)cpp");
  const bool clean = a.pre.diags.empty() && a.diags().empty();
  check(clean, "declaration annotations accept parsed expressions and separate ordinary attributes");
  if (!clean) return;

  const auto role = a.decl(a.its.top.at(0));
  const auto other = a.decl(a.its.top.at(1));
  const auto tag = a.decl(a.its.top.at(2));
  const ast::u32 forward = a.its.top.at(4);
  const ast::u32 definition = a.its.top.at(5);
  const auto owner = a.decl(definition);
  check(a.decl(forward).entity == owner.entity, "class declaration and definition share an entity");
  const auto owner_annotations = a.p->declaration_annotations(owner.entity);
  check(owner_annotations.size() == 3, "class records retain both redeclarations and repeated constants");
  if (owner_annotations.size() == 3) {
    const auto& first = owner_annotations[0];
    const auto& second = owner_annotations[1];
    const auto& third = owner_annotations[2];
    check(first.type == role.type && first.value == second.value && first.item == a.p->id(forward) &&
              second.item == a.p->id(definition), "enum annotation occurrences carry their type and source item");
    check(third.type == tag.type && a.ins.values.is<ast::VAggregate>(third.value),
          "structural aggregate stays typed");
    for (const auto& annotation : owner_annotations) {
      const auto& item = a.its.items[a.p->index(annotation.item)];
      check(a.pre.region.tokens[item.begin + annotation.offset].kind == lex::Tk::equal,
            "annotation source token is the written equals");
    }
  }

  const ast::u32 first_finish = a.its.top.at(6);
  const ast::u32 int_finish = a.its.top.at(7);
  const ast::u32 defined_finish = a.its.top.at(8);
  const ast::u32 invoke = a.its.top.at(9);
  const auto ret = a.get<ast::Return>(a.children(a.get<ast::Compound>(a.body(invoke).tree).stmts)[0].id);
  const auto call = a.get<ast::Call>(ret.value.id);
  check(call.function == a.decl(first_finish).entity && call.function == a.decl(defined_finish).entity,
        "the selected overload shares the annotated declaration and definition");
  const auto finish = a.p->declaration_annotations(call.function);
  const auto different = a.p->declaration_annotations(a.decl(int_finish).entity);
  check(finish.size() == 3 && different.size() == 1, "annotations remain on their selected overload");
  if (finish.size() == 3 && different.size() == 1) {
    check(finish[0].type == role.type && different[0].type == other.type &&
              finish[0].value == different[0].value, "different enums may have equal underlying values");
    check(finish[0].item == finish[1].item && finish[0].offset < finish[1].offset,
          "equal constants on one declaration preserve distinct source positions");
    check(a.w.types.is(finish[2].type, Builtin::int_) && a.w.values.as_signed(finish[2].value) == 7,
          "a call in the payload is evaluated");
    check(a.ins.seqs.size(a.decl(first_finish).annotations) == 2 &&
              a.ins.seqs.size(a.decl(defined_finish).annotations) == 1,
          "each declaration exposes just its written occurrences");
  }
}

void destructor_specialization_declarations() {
  Analysed a(R"cpp(
template<class T> struct Cell { ~Cell() {} };
template<> Cell<int>::~Cell() {}
template<> inline Cell<char>::~Cell() {}
template<> Cell<long>::~Cell();
template<> Cell<long>::~Cell() {}
)cpp");
  const bool clean = a.pre.diags.empty() && a.diags().empty();
  check(clean, "explicit destructor declarations and definitions select their class member");
  if (!clean) return;

  using namespace sema::decl_flag;
  constexpr auto special_definition = specialization | definition | member | destructor;
  const auto noninline = a.decl(a.its.top.at(1));
  const auto inlined = a.decl(a.its.top.at(2));
  const auto declaration = a.decl(a.its.top.at(3));
  const auto defined = a.decl(a.its.top.at(4));
  const auto owner_argument = [&](const ast::DeclSema& d) -> std::optional<Builtin> {
    const auto& owner = a.w.entities.rec(a.w.entities.rec(d.entity).scope);
    if (owner.args.size() != 1 || owner.args.front().kind != ast::TemplateArgKind::type) return std::nullopt;
    return a.w.types.builtin_of(owner.args.front().type);
  };
  check(owner_argument(noninline) == Builtin::int_ && owner_argument(inlined) == Builtin::char_ &&
            owner_argument(defined) == Builtin::long_,
        "each destructor specialization belongs to the selected class instance");
  check(noninline.flags == special_definition, "the non-inline specialization keeps its written flags");
  check(inlined.flags == (special_definition | inline_), "a written inline specialization is inline");
  check(declaration.flags == (specialization | member | destructor) && defined.flags == special_definition &&
            declaration.entity == defined.entity,
        "a written specialization declaration and definition share an entity");
}

void qualified_template_annotation() {
  Analysed a(R"cpp(
namespace effects {
template<class...> struct Set {};
template<class> struct Contract {};
}
namespace fx = effects;
struct [[=1]] Read { unsigned flags : 3; };
[[=fx::Contract<fx::Set<Read>>{}]] int read();
)cpp");
  const bool clean = a.pre.diags.empty() && a.diags().empty();
  check(clean, "a namespace-qualified template annotation and an annotated bit-field class are valid");
  if (!clean) return;

  const auto fn = a.decl(a.its.top.back());
  const auto annotations = a.p->declaration_annotations(fn.entity);
  check(annotations.size() == 1, "one annotation is attached to the function");
  if (annotations.size() != 1) return;

  const auto& annotation = annotations.front();
  const bool aggregate = a.ins.values.is<ast::VAggregate>(annotation.value);
  check(aggregate && a.w.types.is_class(annotation.type), "the payload is a typed class constant");
  if (!aggregate || !a.w.types.is_class(annotation.type)) return;
  const auto klass = a.w.entities.rec(a.w.types.class_of(annotation.type));
  check(klass.templ != ast::EntityId::none && a.ins.names.spelling(klass.name) == "Contract" &&
            a.ins.values.get<ast::VAggregate>(annotation.value).type == annotation.type,
        "the annotated payload retains the concrete namespace class specialization");
}

void parenthesized_annotation_payloads() {
  Analysed a(R"cpp(
int unrelated;
struct Tag { int n; };
[[=Tag{7}, =(Tag{7}), =((Tag{7})),
  =true, =(true), =((true)),
  =7, =(7), =((7))]] void f();
)cpp");
  const bool clean = a.pre.diags.empty() && a.diags().empty();
  check(clean, "parenthesized annotation payloads are constant expressions");
  if (!clean) return;

  const auto annotations = a.p->declaration_annotations(a.decl(a.its.top.back()).entity);
  check(annotations.size() == 9, "all written annotation payloads are retained");
  if (annotations.size() != 9) return;

  const auto tag = a.decl(a.its.top.at(1)).type;
  const bool aggregate = a.ins.values.is<ast::VAggregate>(annotations[0].value);
  check(aggregate && a.ins.values.get<ast::VAggregate>(annotations[0].value).type == tag,
        "the aggregate constant carries the Tag type");

  const auto payloads = std::span{annotations};
  auto same_payload = [](std::span<const ast::DeclarationAnnotation> occurrences, ast::QualType type) {
    return std::ranges::all_of(occurrences, [&](const auto& occurrence) {
      return occurrence.type == type && occurrence.value == occurrences.front().value;
    });
  };
  check(same_payload(payloads.subspan(0, 3), tag), "parentheses preserve the aggregate annotation type and value");
  check(same_payload(payloads.subspan(3, 3), a.w.types.builtin(Builtin::bool_)),
        "parentheses preserve the boolean annotation type and value");
  check(same_payload(payloads.subspan(6, 3), a.w.types.builtin(Builtin::int_)),
        "parentheses preserve the integer annotation type and value");
}

void invalid_declaration_annotations() {
  using sema::Diag;
  struct Witness {
    std::string_view source;
    Diag diagnostic;
  };
  for (const Witness& witness : {
           Witness{"int runtime(); [[=runtime()]] void f();", Diag::not_constant},
           Witness{"[[=void()]] void f();", Diag::annotation_invalid_type},
           Witness{R"cpp([[="literal"]] void f();)cpp", Diag::annotation_invalid_type},
           Witness{"struct Tag { mutable int value; }; [[=Tag{3}]] void f();", Diag::annotation_invalid_type},
           Witness{"int [[=1]] f();", Diag::attributes_not_allowed},
           Witness{"static [[=1]] int f();", Diag::attributes_not_allowed},
           Witness{"int * [[=1]] f();", Diag::attributes_not_allowed},
           Witness{"consteval int f() { return 1; } [[=&f]] void g();", Diag::annotation_invalid_type},
           Witness{"constexpr int n=1; struct R { const int& r; }; [[=R{n}]] void f();", Diag::annotation_not_supported},
           Witness{"[[nodiscard, =3]] int f();", Diag::annotation_mixed_list},
           Witness{"[[=3, nodiscard]] int f();", Diag::annotation_mixed_list},
           Witness{"struct C { friend void f [[=1]] (); };", Diag::attributes_not_allowed},
           Witness{"[[=1]] struct Unattached {};", Diag::attributes_not_allowed},
       }) {
    auto diagnostics = Analysed(witness.source).diags();
    check(std::ranges::any_of(diagnostics, [&](const auto& d) { return d.id == sema::diag_id(witness.diagnostic); }),
          witness.source);
  }
}

void unsupported_bit_fields() {
  for (std::string_view source : {
           "struct F { unsigned a : 0; };",
           "struct F { unsigned a : -1; };",
           "struct F { unsigned a : 3.0; };",
           "int n; struct F { unsigned a : n; };",
           "struct F { unsigned : 1; };",
           "struct F { unsigned (a) : 1; };",
           "struct F { volatile int a : 1; };",
           "using I = const int; struct F { I a : 1; };",
           "struct F { static int a : 1; };",
           "struct F { unsigned a : 1 = 1; };",
           "union F { unsigned a : 1; };",
           "struct B {}; struct F : B { unsigned a : 1; };",
           "template<class T> struct F { unsigned a : 1; };",
           "struct F { template<class T> unsigned a : 1; };",
           "struct __attribute__((packed)) F { unsigned a : 1; };"}) {
    check(!Analysed(source).diags().empty(), source);
  }
  check(Analysed("struct F { unsigned a : 33; };").diags().empty(), "an over-width unsigned bit-field");
  check(Analysed("struct F { long a : 1; };").diags().empty(), "a long bit-field");
  for (std::string_view source : {
           "unsigned* test(F& f) { return &(f.a); }",
           "unsigned* test(F& f) { return &(f.a = 1); }",
           "unsigned* test(F& f) { return &(++f.a); }",
           "auto test(F& f) { return sizeof((0, f.a)); }",
           "unsigned& test(F& f) { return f.a; }",
           "int test(F& f) { const unsigned& r = f.a; return r; }",
           "unsigned test(volatile F& f) { return f.a; }",
           "constexpr F f{3, 4}; static_assert(f.a == 3);",
           "void test(bool b, F& f) { (b ? f.a : f.b) = 1; }"}) {
    check(!Analysed("struct F { unsigned a : 3; unsigned b : 5; }; " + std::string(source)).diags().empty(), source);
  }
}

void gnu_pointer_qualifiers() {
  Analysed a(R"cpp(
#define HEADER_RESTRICT __restrict
int read_value(const int* HEADER_RESTRICT value);
#undef HEADER_RESTRICT

using Reader = int (*)(const int* __restrict__);
static_assert(__is_same(decltype(&read_value), Reader));

extern int* __restrict const volatile qualified;
static_assert(__is_const(decltype(qualified)));
static_assert(__is_volatile(decltype(qualified)));

extern int* __restrict__ * const __restrict nested;
void unnamed(void* __restrict);

int restrict = 1;
int use(const int* __restrict input) {
  const int* __restrict__ copy = input;
  return *copy + restrict;
}
)cpp");
  check(a.pre.diags.empty() && a.diags().empty(), "GNU pointer qualifiers through preprocessing and parsing");

  for (std::string_view invalid : {"int __restrict scalar;", "int* trailing __restrict;", "int __restrict__;"})
    check(!Analysed(invalid).diags().empty(), invalid);

  auto function = Analysed("extern int (* __restrict__ function_ptr)();").diags();
  check(function.size() == 1 && function.front().id == sema::diag_id(sema::Diag::restrict_function_pointer),
        "the qualifier cannot apply directly to a pointer to function");
  auto duplicate = Analysed("extern int* __restrict __restrict__ pointer;").diags();
  check(duplicate.size() == 1 && duplicate.front().id == sema::diag_id(sema::Diag::duplicate_specifier),
        "the two spellings count as one qualifier");
}

void constants() {
  Analysed a(
      "const int N = 2 + 3;\n"
      "int arr[N];\n"
      "long big = 1L << 40;\n"
      "const char* s = \"ab\";\n"
      "unsigned u = -1;\n"
      "int z;\n");
  check(a.diags().empty(), "no diagnostics");
  auto value = [&](ast::u32 k) {
    auto d = a.get<ast::Declarator>(a.children(a.get<ast::DeclGroup>(a.item(k).tree).declarators)[0].id);
    return d.value;
  };
  check(a.w.values.rec(value(0)).bits == 5, "N = 5");
  check(a.spell(a.decl(1).type) == "int[5]", "the bound is N");
  check(a.w.values.rec(value(2)).bits == (1ull << 40), "1L << 40");
  const sema::ValueRec& s = a.w.values.rec(value(3));
  check(s.kind == sema::ValueKind::address && a.w.values.rec(s.base).kind == sema::ValueKind::string,
        "s points to a string literal object");
  check(a.w.values.rec(value(4)).bits == 0xffffffffu, "-1 converted to unsigned");
  check(value(5) == sema::ConstId::none, "zero-initialized");
  check(a.decl(0).linkage == ast::Linkage::internal && a.decl(2).linkage == ast::Linkage::external,
        "a const variable has internal linkage");
}

void assignments() {
  Analysed a("void f() { char c = 1; c += 1; c <<= 2; c = 3; }");
  check(a.diags().empty(), "no diagnostics");
  auto stmts = a.children(a.get<ast::Compound>(a.body(0).tree).stmts);
  auto add = a.get<ast::Assign>(a.get<ast::ExprStmt>(stmts[1].id).expr.id);
  check(add.op == ast::AssignOp::add && a.w.types.is(add.computation, Builtin::int_) &&
            a.w.types.is(add.info.type, Builtin::char_) && add.info.category == ast::ValueCategory::lvalue,
        "c += 1 is computed in int and is an lvalue of type char");
  auto shl = a.get<ast::Assign>(a.get<ast::ExprStmt>(stmts[2].id).expr.id);
  check(a.w.types.is(shl.computation, Builtin::int_), "c <<= 2 is computed in the promoted type");
  auto set = a.get<ast::Assign>(a.get<ast::ExprStmt>(stmts[3].id).expr.id);
  check(a.get<ast::ImplicitConversion>(set.rhs.id).conversion == ConversionKind::integral, "3 converted to char");
  auto decl = a.get<ast::DeclStmt>(stmts[0].id);
  auto var = a.get<ast::LocalVar>(a.children(decl.decls)[0].id);
  check(var.local == 0 && a.ins.seqs.at(a.body(0).locals, 0).decl == a.children(decl.decls)[0].id,
        "the local's record names its declaration");
}

void diagnostics() {
  Analysed a("int main() {\n  return x;\n}\nvoid h() { return 1; }\n");
  auto ds = a.diags();
  check(ds.size() == 2, "two diagnostics");
  check(ds[0].id == sema::diag_id(sema::Diag::undeclared) && ds[0].at.offset == 6,
        "undeclared x, located item-relative: 'int main ( ) { return' are offsets 0-5");
  check(a.ins.names.spelling(ast::NameId(ast::u32(ds[0].args[0].raw))) == "x", "the name is the argument");
  check(ds[1].id == sema::diag_id(sema::Diag::return_value_in_void), "return 1 in a void function");
  check(a.p->byte_offset(ds[0].at) == 22, "rendered to the byte offset of x");
  // Beyond the subset: one diagnostic for the class (virtual bases are not supported yet), none
  // for its members, and no crash while its declared names get placeholder records.
  Analysed c("struct B {}; struct S : virtual B { int x; }; int y = 1;");
  check(c.diags().size() == 1 && c.diags()[0].id == sema::diag_id(sema::Diag::not_supported), "virtual base: one diagnostic");
  // A struct's member missing its `;` is one diagnostic, in the member's item.
  Analysed d("struct S { int x }; int y = 1;");
  check(d.diags().size() == 1 && d.diags()[0].id == sema::diag_id(sema::Diag::expected_token), "struct: one diagnostic");
}

void control_flow() {
  Analysed a("int f(int n) { int s = 0; for (int i = 0; i < n; ++i) { if (i) continue; s += i; } "
             "while (n) { n--; break; } do s++; while (s < 3); return s > 0 ? s : -s; }");
  check(a.diags().empty(), "no diagnostics");
  auto stmts = a.children(a.get<ast::Compound>(a.body(0).tree).stmts);
  check(stmts.size() == 5, "five statements");
  auto loop = a.get<ast::For>(stmts[1].id);
  check(a.is<ast::DeclStmt>(loop.init.id) && a.is<ast::Binary>(loop.condition.id) && a.is<ast::Unary>(loop.increment.id),
        "for: declaration, condition, increment");
  auto w = a.get<ast::While>(stmts[2].id);
  check(a.get<ast::ImplicitConversion>(w.condition.id).conversion == ConversionKind::boolean,
        "while (n): n converted to bool");
  check(a.is<ast::Do>(stmts[3].id), "do statement");
  auto cond = a.get<ast::Conditional>(a.get<ast::Return>(stmts[4].id).value.id);
  check(a.w.types.is(cond.info.type, Builtin::int_) && cond.info.category == ast::ValueCategory::prvalue,
        "s > 0 ? s : -s is an int prvalue");
  check(a.ins.seqs.size(a.body(0).locals) == 3, "n, s and i");
}

// Constant evaluation (#87): the diagnostics a program gets, by kind, in order.
std::vector<sema::Diag> kinds(std::string_view text) {
  Analysed a(text);
  std::vector<sema::Diag> r;
  for (const engine::Diagnostic& d : a.diags()) r.push_back(*sema::sema_diag_of(d.id));
  return r;
}

void constant_evaluation() {
  using enum sema::Diag;
  using V = std::vector<sema::Diag>;
  check(kinds("constexpr int f(int n) { return n <= 1 ? 1 : n * f(n - 1); }\nstatic_assert(f(5) == 120);").empty(),
        "constexpr recursion in a static_assert");
  check(kinds("static_assert(1 + 1 == 3, \"no\");") == V{static_assert_failed_message}, "a failed static_assert");
  // From the review of #87's first commit.
  check(kinds("constexpr int f() { volatile int x = 1; return x; }\nstatic_assert(f() == 1);") == V{not_constant},
        "a volatile read is not constant");
  check(kinds("const volatile int x = 1;\nstatic_assert(x == 1);") == V{not_constant},
        "a const volatile variable is not usable in constant expressions");
  check(kinds("int f(int n) { if consteval { return 1; } else { return n; } }").empty(), "if consteval with else");
  check(kinds("int f(int n) { if consteval { return 1; } else return n; }") == V{expected_token},
        "the statement after else must be compound ([stmt.if]/4)");
  check(kinds("consteval void cv() {}\nint main() { cv(); return 0; }").empty(),
        "an immediate invocation of a void consteval function");
  {
    // From the verification of the review fixes: the void invocation leaves no call behind.
    Analysed a("int g;\nconsteval void cv() { if !consteval { g = 1; } }\nint main() { cv(); return g; }");
    check(a.diags().empty(), "a void immediate invocation");
    auto stmts = a.children(a.get<ast::Compound>(a.body(2).tree).stmts);
    auto e = a.get<ast::ExprStmt>(stmts[0].id).expr.id;
    check(!a.is<ast::Call>(e), "the invocation is replaced, so cv's body does not run at run time");
  }
  check(kinds("int f() { int m = 1; constexpr int x = true ? 1 : m; constexpr int y = false && m; return x + y; }").empty(),
        "a local that is not usable, named but not evaluated");
  check(kinds("int f() { int m = 1; constexpr int x = m; return x; }") == V{not_constant},
        "a local that is not usable, evaluated");
  check(kinds("int f() { int m = 1; constexpr int x = (m = 2); return x; }") == V{not_constant},
        "a local that is not usable, modified");
  check(kinds("int g() { return 1; }\nconstexpr int (*fp)() = g;\nstatic_assert(&g != nullptr);\n"
              "static_assert(fp == &g);").empty(),
        "function addresses are constants");
  check(kinds("static_assert(true, u8\"x\");") == V{unevaluated_string}, "a message with an encoding prefix");
  check(kinds("static_assert(true, 2);") == V{expected_string_literal}, "a message that is not a string");
  check(kinds("constinit int f();") == V{constinit_function}, "constinit on a function");
  check(kinds("bool b = nullptr == nullptr;\nstatic_assert(nullptr == nullptr);").empty(), "nullptr == nullptr");
}

void handler_kinds() {
  Analysed a("void f() { try {} catch (int) {} catch (...) {} }");
  check(a.diags().empty(), "typed and catch-all handlers are valid");
  auto statements = a.children(a.get<ast::Compound>(a.body(0).tree).stmts);
  const auto handlers = a.children(a.get<ast::TryBlock>(statements[0].id).handlers);
  check(handlers.size() == 2 && handlers[0].id != handlers[1].id, "ordered handlers have distinct nodes");
  auto typed = a.get<ast::Handler>(handlers[0].id);
  auto catch_all = a.get<ast::Handler>(handlers[1].id);
  check(typed.kind == ast::HandlerKind::typed && a.w.types.is(typed.type, Builtin::int_), "unnamed typed handler");
  check(catch_all.kind == ast::HandlerKind::catch_all && a.w.types.is(catch_all.type, Builtin::void_),
        "catch-all handler with a stable type placeholder");
}

void class_trivial_copyability() {
  using enum sema::CopyOperation;
  using Operations = std::array<sema::CopyOperation, 4>;
  struct Witness {
    std::string_view label;
    std::string_view source;
    ast::u32 top_level_class;
    std::optional<bool> expected;
    // Copy construction, move construction, copy assignment, move assignment.
    std::optional<Operations> operations = std::nullopt;
  };
  const Witness witnesses[] = {
      {"plain class", "struct Value { int payload; };", 0, true,
       Operations{eligible_trivial, eligible_trivial, eligible_trivial, eligible_trivial}},
      {"user-provided ordinary constructor", "struct Value { Value() {} int payload; };", 0, true},
      {"default member initializer", "struct Value { int payload = 17; };", 0, true},
      {"scalar union", "union Value { unsigned bits; int code; };", 0, true},
      {"member pointer field", "struct Owner; struct Value { int Owner::*payload; };", 1, true},
      {"lvalue-reference field", "struct Value { int& payload; };", 0, true,
       Operations{eligible_trivial, eligible_trivial, ineligible, ineligible}},
      {"rvalue-reference field", "struct Value { int&& payload; };", 0, true,
       Operations{ineligible, eligible_trivial, ineligible, ineligible}},
      {"only copy assignment is eligible", "struct Value { Value(const Value&) = delete; int payload; };", 0, true,
       Operations{ineligible, ineligible, eligible_trivial, ineligible}},
      {"first-declaration defaulted copy constructor",
       "struct Value { Value(const Value&) = default; int payload; };",
       0, true, Operations{eligible_trivial, ineligible, eligible_trivial, ineligible}},
      {"first-declaration defaulted copy assignment is the sole eligible operation",
       "struct Value { Value(const Value&) = delete; Value& operator=(const Value&) = default; int payload; };",
       0, true, Operations{ineligible, ineligible, eligible_trivial, ineligible}},
      {"const scalar array leaves no eligible operation",
       "struct Value { Value(const Value&) = delete; const int payload[2]; };",
       0, false, Operations{ineligible, ineligible, ineligible, ineligible}},
      {"unrelated assignment", "struct Value { Value& operator=(int); };", 0, true,
       Operations{eligible_trivial, eligible_trivial, eligible_trivial, eligible_trivial}},
      // Deleted copies suppress implicit moves. [class.prop]/1 still requires one eligible operation.
      {"no eligible operation",
       "struct Value { Value(const Value&) = delete; Value& operator=(const Value&) = delete; };", 0, false,
       Operations{ineligible, ineligible, ineligible, ineligible}},
      {"user-provided copy constructor", "struct Value { Value(const Value&) {} };", 0, false,
       Operations{eligible_nontrivial, ineligible, eligible_trivial, ineligible}},
      {"user-provided move constructor", "struct Value { Value(Value&&) {} };", 0, false,
       Operations{ineligible, eligible_nontrivial, ineligible, ineligible}},
      {"user-provided destructor", "struct Value { ~Value() {} };", 0, false},
      {"class-valued field", "struct Child { int payload; }; struct Value { Child child; };", 1, std::nullopt},
      {"base class", "struct Base { int payload; }; struct Value : Base {};", 1, std::nullopt},
  };
  for (const Witness& witness : witnesses) {
    Analysed a(witness.source);
    const auto label = std::string(witness.label);
    const bool clean = a.pre.diags.empty() && a.diags().empty();
    check(clean, label + " has no diagnostics");
    if (!clean) continue;
    const auto entity = a.decl(a.its.top.at(witness.top_level_class)).entity;
    const bool defined = a.p->classes().find(entity) != nullptr;
    check(defined, label + " has a class record");
    if (!defined) continue;
    const auto& record = a.p->classes().rec(entity);
    check(record.trivially_copyable() == witness.expected, label + " trivial copyability");
    if (witness.operations) {
      const auto& ops = record.trivial_copyability;
      const Operations actual{ops.copy_constructor, ops.move_constructor, ops.copy_assignment, ops.move_assignment};
      check(actual == *witness.operations, label + " copy and move operations");
    }
  }

  Analysed forward("struct Forward;");
  const bool clean = forward.pre.diags.empty() && forward.diags().empty();
  check(clean, "forward class has no diagnostics");
  if (clean) {
    check(forward.p->classes().find(forward.decl(0).entity) == nullptr, "forward class has no definition record");
  }

  Analysed feature(R"cpp(
#if __has_builtin(__is_trivially_copyable)
#error __is_trivially_copyable advertised before the evaluator is available
#endif
int probe;
)cpp");
  check(feature.pre.diags.empty() && feature.diags().empty(), "__has_builtin(__is_trivially_copyable) remains zero");
}

int main() {
  constant_evaluation();
  class_trivial_copyability();
  conversions_and_locals();
  prototype_parameters_are_not_body_locals();
  redeclarations_and_linkage();
  destructor_specialization_declarations();
  declaration_annotations();
  qualified_template_annotation();
  parenthesized_annotation_payloads();
  invalid_declaration_annotations();
  unsupported_bit_fields();
  gnu_pointer_qualifiers();
  constants();
  assignments();
  diagnostics();
  control_flow();
  handler_kinds();
  return unit::result("unit.sema");
}
