import std;
import ycxx.cm;
import ycxx.unit;

using namespace ycxx::cm;
using ycxx::unit::check;

namespace {

Model minimize_example() {
  Model model;
  const auto pick = model.bool_var();
  const auto x = model.int_var(-4, 7);
  const Term minimum[]{{1, x}, {2, pick.integer}, {0, x}};
  const Term maximum[]{{1, x}};
  model.linear(minimum, Rel::ge, 4);
  model.linear(maximum, Rel::le, 4);
  const Term objective[]{{-1, x}, {5, pick.integer}, {-2, x}};
  model.minimize(objective);
  return model;
}

Model unsat_example() {
  Model model;
  const auto x = model.int_var(0, 1);
  const Term cancellation[]{{1, x}, {-1, x}};
  model.linear(cancellation, Rel::eq, 0);
  model.linear(cancellation, Rel::ge, 1);
  return model;
}

Model constant_example() {
  Model model;
  model.minimize({});
  return model;
}

Model satisfy_example() {
  Model model;
  const auto x = model.int_var(-2, 2);
  const Term pin[]{{3, x}};
  model.linear(pin, Rel::eq, -3);
  return model;
}

void test_export() {
  const auto model = minimize_example();
  const auto text = write_flatzinc(model);
  check(text && *text ==
        "var 0..1: x0 :: output_var;\n"
        "var -4..7: x1 :: output_var;\n"
        "var -21..17: objective;\n"
        "constraint int_lin_le([-2, -1], [x0, x1], -4);\n"
        "constraint int_lin_le([1], [x1], 4);\n"
        "constraint int_lin_eq([5, -3, -1], [x0, x1, objective], 0);\n"
        "solve minimize objective;\n", "canonical FlatZinc with a signed linear objective");
  check(write_flatzinc(model) == text, "same model exports identically");
  check(write_flatzinc(unsat_example()) ==
        "var 0..1: x0 :: output_var;\nconstraint int_eq(0, 1);\nsolve satisfy;\n",
        "empty sums preserve contradictions and discard tautologies");
  check(write_flatzinc(constant_example()) ==
        "var 0..0: objective;\nsolve minimize objective;\n", "constant zero objective");
  check(write_flatzinc(Model{}) == "solve satisfy;\n", "empty model");

  Model wide_domain;
  wide_domain.int_var(0, std::numeric_limits<std::int64_t>::max());
  check(write_flatzinc(wide_domain) == std::unexpected(FlatZincError::domain_out_of_range),
        "64-bit domain rejected by FlatZinc export");

  Model wide_rhs;
  const auto x = wide_rhs.int_var(0, 1);
  const Term term[]{{1, x}};
  wide_rhs.linear(term, Rel::ge, std::numeric_limits<std::int64_t>::min());
  check(write_flatzinc(wide_rhs) == std::unexpected(FlatZincError::row_out_of_range),
        "out-of-range row rejected before negation");
  Model wide_product;
  const auto big = wide_product.int_var(0, 250'000'000);
  const Term product[]{{3, big}};
  wide_product.linear(product, Rel::le, 1);
  check(write_flatzinc(wide_product) == std::unexpected(FlatZincError::row_out_of_range),
        "row rejected when its term product exceeds solver arithmetic");
  Model wide_objective;
  const auto large = wide_objective.int_var(0, 300'000'000);
  const Term expensive[]{{1, large}};
  wide_objective.minimize(expensive);
  check(write_flatzinc(wide_objective) == std::unexpected(FlatZincError::objective_out_of_range),
        "objective row rejected when its intermediate sum exceeds solver arithmetic");
  const Term overflow[]{{std::numeric_limits<std::int64_t>::max(), large}, {1, large}};
  wide_objective.minimize(overflow);
  check(write_flatzinc(wide_objective) == std::unexpected(FlatZincError::objective_out_of_range),
        "duplicate coefficient sum cannot overflow");
}

}  // namespace

int main(int argc, char** argv) {
  if (argc == 3 && std::string_view(argv[1]) == "--emit") {
    const std::string_view example(argv[2]);
    Model model;
    if (example == "minimum") model = minimize_example();
    else if (example == "unsat") model = unsat_example();
    else if (example == "constant") model = constant_example();
    else if (example == "satisfy") model = satisfy_example();
    else return 2;
    const auto text = write_flatzinc(model);
    if (!text) return 1;
    std::print("{}", *text);
    return 0;
  }
  if (argc != 1) return 2;
  test_export();
  return ycxx::unit::result("cm");
}
