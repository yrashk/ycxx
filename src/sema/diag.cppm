// The parser's and sema's diagnostics (query-engine.md §A1.7: diagnostics are values). Each kind
// carries its severity and message as an annotation, so the table is derived by reflection, as
// the lexer's is (ycxx.lex:diag). A diagnostic is located at `Loc{item, token offset}` (ast.md
// §8.2) and its arguments are integers or handles: `{N}` is argument N in decimal, `{N:n}` a
// `NameId`'s spelling, `{N:t}` a `QualType` as written, `{N:k}` a token kind's spelling, `{N:c}`
// a string literal constant's text, `{N:r}` why an evaluation is not constant (`reason_arg`).
export module ycxx.sema:diag;

import std;
import ycxx.engine;
import ycxx.interp;
import ycxx.lex;

export namespace ycxx::sema {

using lex::Severity;

struct message {
  Severity severity;
  char text[120];
};

enum class Diag : std::uint32_t {
  // [gram]: syntax
  expected_token [[=message{Severity::error, "expected '{0:k}'"}]] = 1,
  expected_expression [[=message{Severity::error, "expected an expression"}]],
  type_as_expression [[=message{Severity::error, "unexpected type name '{0:n}': expected an expression"}]],
  elaborated_typedef [[=message{Severity::error, "typedef-name '{0:n}' after a class-key or 'enum'"}]],
  typedef_definition [[=message{Severity::error, "a function definition cannot be declared 'typedef'"}]],
  using_namespace_name [[=message{Severity::error, "using-declaration names the namespace '{0:n}'"}]],
  using_class_member [[=message{Severity::error, "a using-declaration at namespace scope names the class member '{0:n}'"}]],
  attributes_not_allowed [[=message{Severity::error, "an attribute-specifier cannot appear here"}]],
  array_initializer [[=message{Severity::error, "an array is initialized by a braced list or, for characters, a string literal"}]],
  asm_label_conflict [[=message{Severity::error, "'{0:n}' is given another asm label"}]],
  asm_label_string [[=message{Severity::error, "an asm label is an ordinary string literal"}]],
  sizeof_incomplete [[=message{Severity::error, "the size of '{0:t}', which is incomplete or a function type"}]],
  union_reference [[=message{Severity::error, "a union cannot have a reference member '{0:n}'"}]],
  function_definition_in_block [[=message{Severity::error, "a function cannot be defined in a block (is a '}' missing?)"}]],
  extraneous_brace [[=message{Severity::error, "a '}' that closes nothing"}]],
  operator_member_only [[=message{Severity::error, "'{0:n}' must be a member function"}]],
  operator_arity [[=message{Severity::error, "'{0:n}' has the wrong number of parameters"}]],
  operator_needs_class [[=message{Severity::error, "'{0:n}' needs a parameter of class or enumeration type"}]],
  operator_static [[=message{Severity::error, "'{0:n}' cannot be a static member function"}]],
  conversion_decl_type [[=message{Severity::error, "a conversion function cannot specify a separate return type"}]],
  conversion_static [[=message{Severity::error, "a conversion function cannot be static"}]],
  conversion_parameters [[=message{Severity::error, "a conversion function requires an empty parameter list"}]],
  conversion_redeclared [[=message{Severity::error, "redeclaration of conversion function to '{0:t}'"}]],
  invalid_literal_operator_id [[=message{Severity::error, "a literal operator name needs an empty ordinary string and a suffix"}]],
  deprecated_literal_operator_id [[=message{Severity::warning, "a separated suffix in a literal operator name is deprecated"}]],
  literal_operator_scope [[=message{Severity::error, "a literal operator must declare a function belonging to a namespace"}]],
  literal_operator_linkage [[=message{Severity::error, "a literal operator cannot have C language linkage"}]],
  literal_operator_parameters [[=message{Severity::error, "invalid parameters for a literal operator"}]],
  integer_literal_operator_missing [[=message{Severity::error, "no matching integer literal operator '{0:n}'"}]],
  integer_literal_operator_conflict [[=message{Severity::error, "both a raw literal operator and a numeric literal operator template are visible"}]],
  integer_literal_operator_unsupported [[=message{Severity::error, "raw and template integer literal calls are not supported yet"}]],
  enum_no_type [[=message{Severity::error, "no integral type can hold every value of '{0:n}'"}]],
  enum_base_required [[=message{Severity::error, "this declaration of unscoped enumeration '{0:n}' requires an underlying type"}]],
  enum_base_not_integral [[=message{Severity::error, "an enumeration base must name an integral type"}]],
  enum_base_mismatch [[=message{Severity::error, "enumeration '{0:n}' redeclared with a different underlying type"}]],
  enum_scope_mismatch [[=message{Severity::error, "scoped and unscoped declarations of enumeration '{0:n}' conflict"}]],
  enum_redefinition [[=message{Severity::error, "redefinition of enumeration '{0:n}'"}]],
  qualified_opaque_enum [[=message{Severity::error, "an ordinary opaque enumeration declaration requires an unqualified name"}]],
  expected_statement [[=message{Severity::error, "expected a statement"}]],
  expected_declaration [[=message{Severity::error, "expected a declaration"}]],
  expected_type_specifier [[=message{Severity::error, "expected a type specifier"}]],
  expected_declarator [[=message{Severity::error, "expected a declarator"}]],
  expected_unqualified_id [[=message{Severity::error, "expected an identifier"}]],
  unexpected_token [[=message{Severity::error, "unexpected '{0:k}'"}]],
  unexpected_identifier [[=message{Severity::error, "unexpected identifier '{0:n}'"}]],
  not_supported [[=message{Severity::error, "'{0:k}' is not supported yet"}]],
  extended_float_not_supported [[=message{Severity::error, "extended floating-point types are not supported yet"}]],
  static_member_definition_not_supported [[=message{Severity::error, "defining a static data member of a class template outside the class is not supported yet"}]],
  extraneous_template_head [[=message{Severity::error, "extraneous template parameter list: only a member template of a class template is declared after two"}]],
  // [dcl.spec], [dcl.type]
  duplicate_specifier [[=message{Severity::error, "duplicate '{0:k}'"}]],
  conflicting_specifiers [[=message{Severity::error, "'{0:k}' cannot be combined with the other type specifiers"}]],
  storage_class_conflict [[=message{Severity::error, "a declaration has at most one storage class specifier"}]],
  missing_type_specifier [[=message{Severity::error, "a declaration needs a type specifier"}]],
  // [dcl.spec.auto]
  auto_uninitialized [[=message{Severity::error, "'{0:n}' has a placeholder type and no initializer to deduce it from"}]],
  auto_deduction_failed [[=message{Severity::error, "cannot deduce '{0:t}' from an initializer of type '{1:t}'"}]],
  auto_inconsistent [[=message{Severity::error, "'{0:t}' deduced as '{1:t}' here and as '{2:t}' before"}]],
  auto_list [[=message{Severity::error, "cannot deduce a placeholder type from a braced list of {0} elements"}]],
  auto_used_in_initializer [[=message{Severity::error, "'{0:n}' is used in its own initializer, which deduces its type"}]],
  auto_undeduced [[=message{Severity::error, "'{0:n}' is used before its return type is deduced"}]],
  decltype_auto_alone [[=message{Severity::error, "'decltype(auto)' must be the whole declared type"}]],
  auto_not_allowed [[=message{Severity::error, "a placeholder type is not allowed here"}]],
  binding_requires_auto [[=message{Severity::error, "a structured binding declaration requires 'auto'"}]],
  binding_requires_initializer [[=message{Severity::error, "a structured binding declaration requires an initializer"}]],
  binding_unsupported_type [[=message{Severity::error, "structured bindings for initializer type '{0:t}' are not supported yet"}]],
  binding_arity [[=message{Severity::error, "structured binding declares {0} names for an array with {1} elements"}]],
  not_captured [[=message{Severity::error, "'{0:n}' is not captured: the lambda has no capture-default"}]],
  cannot_capture [[=message{Severity::error, "'{0:n}' cannot be captured: it is not a local variable"}]],
  namespace_lambda_capture_default [[=message{Severity::error, "a namespace-scope lambda cannot have a capture-default"}]],
  namespace_lambda_init_capture [[=message{Severity::error, "init-capture in a namespace-scope lambda is not supported yet"}]],
  namespace_lambda_generic [[=message{Severity::error, "generic namespace-scope lambdas are not supported yet"}]],
  namespace_lambda_init [[=message{Severity::error, "a namespace-scope lambda must directly initialize the variable in this subset"}]],
  capture_twice [[=message{Severity::error, "'{0:n}' is captured twice"}]],
  closure_assignment [[=message{Severity::error, "'{0:t}' has a lambda-capture: its copy assignment is deleted"}]],
  capture_redundant [[=message{Severity::error, "'{0:n}' is captured the way the capture-default already captures"}]],
  this_not_captured [[=message{Severity::error, "'this' is not captured: the lambda has no capture-default"}]],
  trailing_return_needs_auto [[=message{Severity::error, "a function with a trailing return type is declared with 'auto' alone"}]],
  // [dcl.decl], [dcl.fct], [dcl.init]
  void_variable [[=message{Severity::error, "variable '{0:n}' has incomplete type 'void'"}]],
  void_parameter [[=message{Severity::error, "a parameter cannot have type 'void'"}]],
  bound_not_constant [[=message{Severity::error, "an array bound must be a positive integral constant expression"}]],
  invalid_array_element [[=message{Severity::error, "an array element cannot have type '{0:t}'"}]],
  bit_field_width [[=message{Severity::error, "a bit-field width must be an integral constant expression between 1 and 32"}]],
  bit_field_declaration [[=message{Severity::error, "only named, unqualified int and unsigned int bit-fields in ordinary records are supported"}]],
  bit_field_initializer [[=message{Severity::error, "a default member initializer on a bit-field is not supported yet"}]],
  bit_field_address [[=message{Severity::error, "a bit-field has no address"}]],
  bit_field_size [[=message{Severity::error, "sizeof cannot be applied to a bit-field"}]],
  bit_field_reference [[=message{Severity::error, "binding a reference to a bit-field is not supported yet"}]],
  volatile_bit_field [[=message{Severity::error, "access to a volatile bit-field is not supported yet"}]],
  bit_field_conditional [[=message{Severity::error, "a conditional expression designating a bit-field is not supported yet"}]],
  restrict_function_pointer [[=message{Severity::error, "a pointer to a function cannot be restrict-qualified"}]],
  restrict_member_pointer [[=message{Severity::error, "a pointer to member cannot be restrict-qualified"}]],
  member_pointer_owner [[=message{Severity::error, "a pointer-to-member declarator must name a class"}]],
  member_pointer_type [[=message{Severity::error, "cannot form a pointer to a member of type '{0:t}'"}]],
  member_pointer_values [[=message{Severity::error, "this use of a pointer-to-member value is not supported yet"}]],
  pointer_to_reference [[=message{Severity::error, "cannot form a pointer to a reference type '{0:t}'"}]],
  indirection_to_qualified_function [[=message{Severity::error, "cannot form a pointer or reference to qualified function type '{0:t}'"}]],
  noexcept_narrowing [[=message{Severity::error, "narrowing conversion from '{0:t}' to 'bool' in a noexcept-specifier"}]],
  function_returns_function [[=message{Severity::error, "a function cannot return a function or an array"}]],
  function_initialized [[=message{Severity::error, "function '{0:n}' cannot have an initializer"}]],
  new_requires_object [[=message{Severity::error, "cannot allocate an object of type '{0:t}'"}]],
  new_const_uninitialized [[=message{Severity::error, "new of const type '{0:t}' requires an initializer"}]],
  delete_requires_pointer [[=message{Severity::error, "cannot delete an expression of type '{0:t}'; a pointer to object is required"}]],
  allocation_declaration [[=message{Severity::error, "unsupported declaration of '{0:n}'; expected an ordinary global allocation or deallocation function"}]],
  const_uninitialized [[=message{Severity::error, "const variable '{0:n}' must be initialized"}]],
  redefinition [[=message{Severity::error, "redefinition of '{0:n}'"}]],
  default_arg_redefined [[=message{Severity::error, "redefinition of a default argument"}]],
  parameter_in_default_argument [[=message{Severity::error, "parameter '{0:n}' may not appear in a potentially evaluated default argument"}]],
  default_arg_missing [[=message{Severity::error, "missing default argument on a parameter after one with a default"}]],
  default_arg_dependent [[=message{Severity::error, "default arguments of templates are not supported yet"}]],
  default_arg_template_redeclared [[=message{Severity::error, "a redeclaration of a function template cannot add default arguments"}]],
  default_arg_not_allowed [[=message{Severity::error, "a default argument is not allowed here"}]],
  conflicting_declaration [[=message{Severity::error, "conflicting declaration of '{0:n}'"}]],
  expected_namespace_name [[=message{Severity::error, "expected a namespace name"}]],
  nested_namespace_inline [[=message{Severity::error, "a nested namespace definition cannot be 'inline'"}]],
  nested_namespace_attributes [[=message{Severity::error, "a nested namespace definition takes no attributes"}]],
  overload_not_supported [[=message{Severity::error, "overloading '{0:n}' is not supported yet"}]],
  linkage_conflict [[=message{Severity::error, "'{0:n}' was declared with a different language linkage"}]],
  unknown_linkage [[=message{Severity::error, "unknown language linkage; only \"C\" and \"C++\" are known"}]],
  nonconstant_initializer [[=message{Severity::error, "dynamic initialization of a namespace-scope variable is not supported yet: {0:r}"}]],
  // [dcl.constexpr], [dcl.pre], [expr.const]
  not_constant [[=message{Severity::error, "not a constant expression: {0:r}"}]],
  static_assert_failed [[=message{Severity::error, "static assertion failed"}]],
  static_assert_failed_message [[=message{Severity::error, "static assertion failed: {0:c}"}]],
  constexpr_uninitialized [[=message{Severity::error, "constexpr variable '{0:n}' must be initialized"}]],
  constexpr_specifiers [[=message{Severity::error, "at most one of 'constexpr', 'consteval' and 'constinit' may appear"}]],
  consteval_not_function [[=message{Severity::error, "'consteval' can only declare a function"}]],
  constinit_automatic [[=message{Severity::error, "'constinit' can only declare a variable with static storage duration"}]],
  constinit_function [[=message{Severity::error, "'constinit' can only declare a variable"}]],
  expected_string_literal [[=message{Severity::error, "expected a string literal"}]],
  unevaluated_string [[=message{Severity::error, "this string cannot have an encoding prefix or a suffix"}]],
  consteval_redeclared [[=message{Severity::error, "'{0:n}' is declared both 'consteval' and not"}]],
  constexpr_redeclared [[=message{Severity::error, "'{0:n}' is declared both 'constexpr' and not"}]],
  // [basic.start.main]
  main_return_type [[=message{Severity::error, "'main' must return 'int'"}]],
  main_parameters [[=message{Severity::error, "'main' must have no parameters or (int, char**)"}]],
  main_specifier [[=message{Severity::error, "'main' cannot be declared '{0:k}'"}]],
  main_not_function [[=message{Severity::error, "'main' at global scope must be a function"}]],
  main_named [[=message{Severity::error, "'main' cannot be used in an expression"}]],
  main_linkage [[=message{Severity::error, "'main' cannot have C language linkage"}]],
  // [basic.lookup.unqual]
  undeclared [[=message{Severity::error, "use of undeclared identifier '{0:n}'"}]],
  // [expr]
  not_assignable [[=message{Severity::error, "expression is not assignable"}]],
  assign_to_const [[=message{Severity::error, "cannot assign to a variable of const-qualified type '{0:t}'"}]],
  invalid_operands [[=message{Severity::error, "invalid operands to binary expression ('{0:t}' and '{1:t}')"}]],
  invalid_operand [[=message{Severity::error, "invalid argument type '{0:t}' to unary expression"}]],
  increment_bool [[=message{Severity::error, "ISO C++17 does not allow incrementing or decrementing a bool"}]],
  not_lvalue_address [[=message{Severity::error, "cannot take the address of an rvalue of type '{0:t}'"}]],
  indirection_not_pointer [[=message{Severity::error, "indirection requires a pointer operand ('{0:t}' invalid)"}]],
  not_callable [[=message{Severity::error, "called object of type '{0:t}' is not a function"}]],
  too_few_arguments [[=message{Severity::error, "too few arguments in call: expected {0}, have {1}"}]],
  too_many_arguments [[=message{Severity::error, "too many arguments in call: expected {0}, have {1}"}]],
  no_conversion [[=message{Severity::error, "cannot convert '{0:t}' to '{1:t}'"}]],
  not_contextually_bool [[=message{Severity::error, "'{0:t}' cannot be converted to bool"}]],
  integer_literal_too_large [[=message{Severity::error, "integer literal is too large for any of its types"}]],
  void_value [[=message{Severity::error, "a void expression has no value"}]],
  incompatible_operands [[=message{Severity::error, "incompatible operand types ('{0:t}' and '{1:t}')"}]],
  pointer_arithmetic_void [[=message{Severity::error, "arithmetic on a pointer to an incomplete or function type '{0:t}'"}]],
  divide_by_zero [[=message{Severity::error, "division by zero in a constant expression"}]],
  // [stmt]
  break_outside_loop [[=message{Severity::error, "'break' outside a loop or switch"}]],
  continue_outside_loop [[=message{Severity::error, "'continue' outside a loop"}]],
  return_value_in_void [[=message{Severity::error, "a void function cannot return a value"}]],
  return_missing_value [[=message{Severity::error, "a non-void function must return a value"}]],
  redeclared_local [[=message{Severity::error, "redefinition of '{0:n}'"}]],
  case_outside_switch [[=message{Severity::error, "'{0:k}' label not in a switch statement"}]],
  duplicate_case [[=message{Severity::error, "duplicate case value"}]],
  duplicate_default [[=message{Severity::error, "multiple default labels in one switch"}]],
  switch_not_integral [[=message{Severity::error, "switch condition of type '{0:t}' is not an integer or enumeration"}]],
  undeclared_label [[=message{Severity::error, "use of undeclared label '{0:n}'"}]],
  redefined_label [[=message{Severity::error, "redefinition of label '{0:n}'"}]],
  jump_past_init [[=message{Severity::error, "the jump bypasses the initialization of a variable"}]],
  condition_specifier [[=message{Severity::error, "'{0:k}' is not allowed in a condition"}]],
  condition_declarator [[=message{Severity::error, "a condition cannot declare a function or an array"}]],
  // [class], [class.mem], [expr.ref], [dcl.init.aggr], [dcl.init.list]
  incomplete_type [[=message{Severity::error, "'{0:t}' is an incomplete type"}]],
  no_member [[=message{Severity::error, "no member named '{0:n}' in '{1:t}'"}]],
  // [class.derived], [class.mi]
  base_not_class [[=message{Severity::error, "a base must be a class; '{0:t}' is not one"}]],
  duplicate_base [[=message{Severity::error, "base class '{0:t}' named more than once"}]],
  union_with_base [[=message{Severity::error, "a union cannot have base classes"}]],
  ambiguous_base [[=message{Severity::error, "'{0:t}' is an ambiguous base of '{1:t}'"}]],
  inaccessible_base [[=message{Severity::error, "'{0:t}' is an inaccessible base of '{1:t}'"}]],
  ambiguous_member [[=message{Severity::error, "member '{0:n}' found in more than one base of '{1:t}'"}]],
  // [class.access]
  private_member [[=message{Severity::error, "'{0:n}' is a private member of '{1:t}'"}]],
  protected_member [[=message{Severity::error, "'{0:n}' is a protected member of '{1:t}'"}]],
  member_of_non_class [[=message{Severity::error, "member reference base type '{0:t}' is not a class"}]],
  arrow_of_non_pointer [[=message{Severity::error, "'->' needs a pointer to a class; '{0:t}' is not one"}]],
  redeclared_member [[=message{Severity::error, "redeclaration of member '{0:n}'"}]],
  too_many_initializers [[=message{Severity::error, "too many initializers for '{0:t}'"}]],
  narrowing [[=message{Severity::error, "narrowing conversion from '{0:t}' to '{1:t}' in a braced initializer"}]],
  braces_for_scalar [[=message{Severity::error, "too many braces around the initializer of '{0:t}'"}]],
  bad_designator [[=message{Severity::error, "designator '{0:n}' does not name a later member of '{1:t}'"}]],
  not_a_type [[=message{Severity::error, "'{0:n}' does not name a class"}]],
  const_member_assign [[=message{Severity::error, "'{0:t}' has a const member, so its copy assignment is deleted"}]],
  unsupported_copy_assignment [[=message{Severity::error, "implicit copy assignment of '{0:t}' is unsupported for this operation"}]],
  mixed_designators [[=message{Severity::error, "either every initializer clause is designated or none is"}]],
  unknown_bound_member [[=message{Severity::error, "a non-static data member cannot be an array of unknown bound"}]],
  class_redefinition [[=message{Severity::error, "redefinition of class '{0:n}'"}]],
  // [class.mfct], [class.this], [class.static]
  this_outside_member [[=message{Severity::error, "'this' can only be used in a non-static member function"}]],
  member_without_object [[=message{Severity::error, "'{0:n}' is a non-static member: it needs an object"}]],
  const_member_call [[=message{Severity::error, "'{0:n}' lacks the const or volatile qualifier of the object it is called on"}]],
  no_viable_function [[=message{Severity::error, "no matching function for call to '{0:n}'"}]],
  ambiguous_call [[=message{Severity::error, "call to '{0:n}' is ambiguous"}]],
  deleted_function [[=message{Severity::error, "'{0:n}' is a deleted function"}]],
  no_viable_constructor [[=message{Severity::error, "no matching constructor for '{0:t}'"}]],
  ambiguous_constructor [[=message{Severity::error, "constructing '{0:t}' is ambiguous"}]],
  deleted_constructor [[=message{Severity::error, "constructing '{0:t}' calls a deleted constructor"}]],
  defaulted_copy_declaration [[=message{Severity::error, "only an ordinary in-class copy constructor taking C& or const C& can be explicitly defaulted here"}]],
  defaulted_copy_unsupported [[=message{Severity::error, "defaulted copy construction of '{0:t}' is not supported for this class"}]],
  reference_uninitialized [[=message{Severity::error, "a reference must be initialized"}]],
  bind_to_lvalue [[=message{Severity::error, "an rvalue reference to '{0:t}' cannot bind to an lvalue"}]],
  bind_to_rvalue [[=message{Severity::error, "a non-const lvalue reference to '{0:t}' cannot bind to a temporary"}]],
  bind_drops_qualifiers [[=message{Severity::error, "binding a reference to '{1:t}' to a value of type '{0:t}' drops qualifiers"}]],
  overload_address [[=message{Severity::error, "the address of the overloaded function '{0:n}' needs a target type"}]],
  immediate_address [[=message{Severity::error, "the address of immediate function '{0:n}' is outside an immediate invocation or context"}]],
  overload_address_none [[=message{Severity::error, "no function '{0:n}' has the target type '{1:t}'"}]],
  overload_address_ambiguous [[=message{Severity::error, "the address of the overloaded function '{0:n}' is ambiguous"}]],
  ambiguous_name [[=message{Severity::error, "reference to '{0:n}' is ambiguous"}]],
  no_namespace_member [[=message{Severity::error, "no member named '{0:n}' in the namespace"}]],
  no_friend_match [[=message{Severity::error, "friend declaration of '{0:n}' matches no earlier declaration"}]],
  not_a_namespace [[=message{Severity::error, "'{0:n}' is not a namespace or class"}]],
  static_outside_class [[=message{Severity::error, "'static' can only be specified inside the class definition"}]],
  static_constructor [[=message{Severity::error, "a constructor cannot be 'static'"}]],
  qualified_constructor [[=message{Severity::error, "a constructor cannot have a cv- or ref-qualifier"}]],
  constructor_namespace [[=message{Severity::error, "constructor definition is outside a namespace enclosing its class"}]],
  no_member_declared [[=message{Severity::error, "'{1:t}' declares no member '{0:n}' with this type"}]],
  cv_on_non_member [[=message{Severity::error, "only a non-static member function can have a cv-qualifier"}]],
  bound_member_not_called [[=message{Severity::error, "a reference to the non-static member function '{0:n}' must be called"}]],
  // [temp]
  invalid_template_parameter_type [[=message{Severity::error, "a non-type template parameter must have a structural type"}]],
  template_args_mismatch [[=message{Severity::error, "the template arguments do not match the parameters of '{0:n}'"}]],
  not_a_template [[=message{Severity::error, "'{0:n}' is not a template"}]],
  instantiation_depth [[=message{Severity::error, "template instantiations nest more than {0} deep"}]],
  nested_too_deeply [[=message{Severity::error, "nested too deeply to analyse: half of the stack is in use"}]],
  default_not_last [[=message{Severity::error, "a template parameter after one with a default argument needs a default argument too"}]],
  default_redefined [[=message{Severity::error, "a template parameter's default argument is given again"}]],
  member_of_function_type [[=message{Severity::error, "a data member cannot have function type '{0:t}'"}]],
  no_template_matches [[=message{Severity::error, "no template named '{0:n}' has this specialization"}]],
  ambiguous_specialization [[=message{Severity::error, "the specialization of '{0:n}' is ambiguous"}]],
  specialized_after_use [[=message{Severity::error, "'{0:n}' is explicitly specialized after its first use"}]],
  partial_same_as_primary [[=message{Severity::error, "the partial specialization of '{0:n}' has the primary template's own arguments"}]],
  partial_argument [[=message{Severity::error, "a specialized non-type argument of '{0:n}' must be a template parameter or not depend on one"}]],
  partial_argument_type [[=message{Severity::error, "the type of a specialized non-type argument of '{0:n}' depends on a parameter of the partial specialization"}]],
  ambiguous_partial [[=message{Severity::error, "more than one partial specialization of '{0:n}' matches, and none is more specialized"}]],
  partial_not_more_specialized [[=message{Severity::error, "the partial specialization of '{0:n}' is not more specialized than its primary template"}]],
  partial_not_deducible [[=message{Severity::error, "a parameter of the partial specialization of '{0:n}' cannot be deduced from its arguments"}]],
  specialization_needs_arguments [[=message{Severity::error, "the explicit specialization of '{0:n}' needs template arguments"}]],
  member_of_explicit_specialization [[=message{Severity::error, "a member of the explicit specialization '{0:n}' is defined without 'template<>'"}]],
  pack_length_mismatch [[=message{Severity::error, "the packs of an expansion have different lengths"}]],
  not_a_pack [[=message{Severity::error, "'{0:n}' is not a parameter pack"}]],
  unexpanded_pack [[=message{Severity::error, "the parameter pack '{0:n}' is used without '...'"}]],
  expansion_without_pack [[=message{Severity::error, "the pattern of a pack expansion names no parameter pack"}]],
  empty_fold [[=message{Severity::error, "a fold over an empty pack needs an initial value for this operator"}]],
  pack_not_last [[=message{Severity::error, "a template parameter pack is followed by a parameter that is neither deduced nor defaulted"}]],
  member_named_as_class [[=message{Severity::error, "a member type cannot have the name of its class '{0:n}'"}]],
  trait_not_applicable [[=message{Severity::error, "this type trait does not apply to '{0:t}'"}]],
  constraint_not_bool [[=message{Severity::error, "an atomic constraint must be of type bool, not '{0:t}'"}]],
  constraint_not_constant [[=message{Severity::error, "an atomic constraint is not a constant expression: {0:r}"}]],
  constraints_not_satisfied [[=message{Severity::error, "the constraints of '{0:n}' are not satisfied"}]],
  not_a_concept [[=message{Severity::error, "'{0:n}' is not a concept"}]],
  concept_constrained [[=message{Severity::error, "the concept '{0:n}' cannot have associated constraints"}]],
  // A GNU built-in function the SDK's or libycxx's headers use and ycxx does not have yet
  // (docs/EXTENSIONS.md), named where lookup finds nothing else.
  builtin_unsupported [[=message{Severity::error, "built-in function '{0:n}' is not supported yet"}]],
  builtin_nan_string [[=message{Severity::error, "the argument of __builtin_nan must be a string literal spelling an integer"}]],
  va_start_context [[=message{Severity::error, "va_start requires an enclosing variadic function"}]],
  va_list_operand [[=message{Severity::error, "expected a mutable va_list cursor, got '{0:t}'"}]],
  va_arg_type [[=message{Severity::error, "va_arg type '{0:t}' is outside the supported x86-64 System V integer and pointer subset"}]],
  friend_c_linkage [[=message{Severity::error, "C-linkage friend function definitions are not supported yet"}]],
};

// A `{N:r}` argument: why an evaluation was not constant. 0 is an expression the front end
// cannot turn into L1 yet; otherwise the interpreter's reason plus one, and its argument above.
constexpr std::uint64_t reason_arg(const interp::Failure& f) {
  return (f.arg << 8) | (std::uint64_t(std::to_underlying(f.reason)) + 1);
}
inline constexpr std::uint64_t reason_unsupported = 0;
// Whether an expression that was not constant has run-time code: the front end made its IR, and
// the evaluation stopped at what it does not or may not evaluate, not at one of its own limits or
// an internal error. An initializer the interpreter cannot evaluate yet (a class temporary) is
// then taken to be dynamic, even where a full evaluator would find it constant.
constexpr bool has_runtime_code(std::uint64_t raw) {
  if (raw == reason_unsupported) return false;
  using enum interp::Reason;
  interp::Reason r = interp::Reason(std::uint8_t((raw & 0xff) - 1));
  return r != ill_formed && r != step_limit && r != depth_limit;
}

inline std::string spell_reason(std::uint64_t raw) {
  if (raw == reason_unsupported) return "the expression cannot be evaluated yet";
  return interp::message({interp::Reason(std::uint8_t((raw & 0xff) - 1)), raw >> 8, {}});
}

inline constexpr std::uint32_t sema_diag_base = 0x2000;

constexpr engine::DiagId diag_id(Diag d) { return engine::DiagId(sema_diag_base + std::to_underlying(d)); }

consteval auto diag_enumerators() { return std::define_static_array(std::meta::enumerators_of(^^Diag)); }

// The kind of a DiagId in sema's range.
constexpr std::optional<Diag> sema_diag_of(engine::DiagId id) {
  std::uint32_t v = std::to_underlying(id);
  std::optional<Diag> r;
  template for (constexpr auto e : diag_enumerators()) {
    if (v == sema_diag_base + std::to_underlying([:e:])) r = [:e:];
  }
  return r;
}

constexpr message message_of(Diag d) {
  message m{};
  template for (constexpr auto e : diag_enumerators()) {
    if (d == [:e:]) m = std::meta::extract<message>(std::meta::annotations_of_with_type(e, ^^message)[0]);
  }
  return m;
}

// Whether a diagnostic is a limit of ycxx's rather than of the program: something not supported
// yet, or the instantiation depth. Where an error means "not satisfied" (a requirement, an atomic
// constraint, [temp.constr.atomic]/3) such a diagnostic stays an error, so a missing feature never
// silently picks another overload.
constexpr bool is_limit(engine::DiagId id) {
  auto d = sema_diag_of(id);
  if (!d) return false;
  if (*d == Diag::instantiation_depth) return true;
  message m = message_of(*d);
  return std::string_view(m.text).contains("not supported");
}
// Whether a diagnostic stays an error where a substitution failure means "not satisfied": a limit
// of ycxx's, an atomic constraint that is not a `bool` constant ([temp.constr.atomic]/3), or a
// return-type-requirement that names no concept, ill-formed whatever the arguments.
constexpr bool survives_substitution(engine::DiagId id) {
  return is_limit(id) || id == diag_id(Diag::constraint_not_bool) || id == diag_id(Diag::constraint_not_constant) ||
         id == diag_id(Diag::not_a_concept);
}

constexpr std::string_view diag_name(Diag d) {
  std::string_view r;
  template for (constexpr auto e : diag_enumerators()) {
    if (d == [:e:]) r = std::define_static_string(std::meta::identifier_of(e));
  }
  return r;
}

// Renders a message; `spell(kind, raw)` spells a `n` (name) or `t` (type) argument.
template <class Spell>
std::string render(Diag d, std::span<const std::uint64_t> args, Spell&& spell) {
  std::string_view t(message_of(d).text);
  std::string out;
  for (std::size_t i = 0; i < t.size(); ++i) {
    if (t[i] != '{' || i + 2 >= t.size()) {
      out += t[i];
      continue;
    }
    std::size_t close = t.find('}', i);
    std::string_view spec = t.substr(i + 1, close - i - 1);
    std::size_t n = std::size_t(spec[0] - '0');
    std::uint64_t v = n < args.size() ? args[n] : 0;
    char how = spec.size() > 2 ? spec[2] : 'd';
    if (how == 'k') out += lex::spelling(lex::Tk(v));
    else if (how == 'n' || how == 't' || how == 'c' || how == 'r') out += spell(how, v);
    else out += std::format("{}", v);
    i = close;
  }
  return out;
}

}  // namespace ycxx::sema
