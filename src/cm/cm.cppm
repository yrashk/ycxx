// Optimizer design §4.1. Model handles refer to variables in insertion order.
module;
#include <stdckdint.h>
export module ycxx.cm;

import std;

export namespace ycxx::cm {

enum class IntVar : std::size_t { none = std::numeric_limits<std::size_t>::max() };
struct BoolVar {
  IntVar integer = IntVar::none;
};

struct Domain {
  std::int64_t lo;
  std::int64_t hi;
};
struct Term {
  std::int64_t coefficient;
  IntVar variable = IntVar::none;
};
enum class Rel { le, eq, ge };

struct Linear {
  std::vector<Term> terms;
  Rel relation;
  std::int64_t rhs;
};

class Model {
public:
  IntVar int_var(std::int64_t lo, std::int64_t hi) pre(lo <= hi) {
    const auto id = IntVar(domains_.size());
    domains_.push_back({lo, hi});
    return id;
  }
  BoolVar bool_var() { return {int_var(0, 1)}; }

  void linear(std::span<const Term> terms, Rel relation, std::int64_t rhs)
      pre(has_variables(terms)) {
    rows_.push_back({std::vector<Term>(terms.begin(), terms.end()), relation, rhs});
  }

  // An empty objective is a constant zero minimization. Omitting minimize asks for satisfaction.
  void minimize(std::span<const Term> terms) pre(has_variables(terms)) {
    objective_ = std::vector<Term>(terms.begin(), terms.end());
  }

  std::span<const Domain> domains() const { return domains_; }
  std::span<const Linear> rows() const { return rows_; }
  const std::optional<std::vector<Term>>& objective() const { return objective_; }

private:
  bool has_variables(std::span<const Term> terms) const {
    return std::ranges::all_of(terms, [this](Term term) {
      return std::to_underlying(term.variable) < domains_.size();
    });
  }

  std::vector<Domain> domains_;
  std::vector<Linear> rows_;
  std::optional<std::vector<Term>> objective_;
};

enum class FlatZincError { domain_out_of_range, row_out_of_range, objective_out_of_range };

// Offline interchange with the FlatZinc solvers in the optimization design. Model values
// are signed 64-bit. Export can fail when a row needs wider solver arithmetic.
std::expected<std::string, FlatZincError> write_flatzinc(const Model& model);

// A Boolean variable's benefit and the maximum number selected in the prefix ending there.
// Prefix capacities must be nondecreasing and no greater than their prefix lengths.
struct PrefixChoice {
  std::uint32_t capacity;
  std::uint32_t benefit;
  bool operator==(const PrefixChoice&) const = default;
};

enum class PrefixFailure { unsupported, budget_exhausted, stopped };

// The same prefix constraints in the portable linear representation, for offline validation.
Model prefix_model(std::span<const PrefixChoice> choices);

// Exact selection under nested prefix constraints. The budget counts candidate visits, and
// equal benefits favor earlier candidates. Return the selected indices in input order.
std::expected<std::vector<std::size_t>, PrefixFailure> solve_prefix_choices(
    std::span<const PrefixChoice> choices, std::size_t budget, std::inplace_stop_token stop = {});

}  // namespace ycxx::cm

namespace ycxx::cm {

Model prefix_model(std::span<const PrefixChoice> choices) {
  Model model;
  std::vector<Term> prefix, objective;
  for (const auto choice : choices) {
    const auto var = model.bool_var().integer;
    prefix.push_back({1, var});
    model.linear(prefix, Rel::le, choice.capacity);
    objective.push_back({-std::int64_t(choice.benefit), var});
  }
  model.minimize(objective);
  return model;
}

std::expected<std::vector<std::size_t>, PrefixFailure> solve_prefix_choices(
    std::span<const PrefixChoice> choices, std::size_t budget, std::inplace_stop_token stop) {
  if (stop.stop_requested()) return std::unexpected(PrefixFailure::stopped);
  if (choices.size() > budget) return std::unexpected(PrefixFailure::budget_exhausted);

  const auto worse_first = [choices](std::size_t a, std::size_t b) {
    if (choices[a].benefit != choices[b].benefit) return choices[a].benefit > choices[b].benefit;
    return a < b;
  };
  std::vector<std::size_t> accepted;
  std::uint32_t capacity = 0;
  for (std::size_t i = 0; i < choices.size(); ++i) {
    if (stop.stop_requested()) return std::unexpected(PrefixFailure::stopped);
    const auto choice = choices[i];
    if (choice.capacity < capacity || choice.capacity > i + 1)
      return std::unexpected(PrefixFailure::unsupported);
    capacity = choice.capacity;
    if (choice.benefit == 0) continue;
    accepted.push_back(i);
    std::ranges::push_heap(accepted, worse_first);
    // Inserting this choice can only violate the current prefix's bound. Dropping the
    // least beneficial choice restores feasibility and is the optimal exchange on a chain.
    if (accepted.size() > capacity) {
      std::ranges::pop_heap(accepted, worse_first);
      accepted.pop_back();
    }
  }
  std::ranges::sort(accepted);
  if (stop.stop_requested()) return std::unexpected(PrefixFailure::stopped);
  return accepted;
}

namespace {

// Chuffed reserves ±500,000,000 as domain sentinels. Leave room for them, as well
// as for intermediate arithmetic in the standard int_lin predicates.
constexpr std::int64_t limit = 499'999'999;

constexpr bool in_range(std::int64_t n) { return -limit <= n && n <= limit; }

struct Expression {
  std::vector<Term> terms;
  Domain bounds{0, 0};
  std::int64_t magnitude = 0;
};

std::optional<Expression> expression(std::span<const Term> terms,
                                     std::span<const Domain> domains) {
  std::vector<Term> ordered(terms.begin(), terms.end());
  std::ranges::stable_sort(ordered, {}, &Term::variable);

  Expression result;
  for (std::size_t pos = 0; pos < ordered.size();) {
    const auto variable = ordered[pos].variable;
    std::int64_t coefficient = 0;
    do {
      if (ckd_add(&coefficient, coefficient, ordered[pos].coefficient)) return std::nullopt;
      ++pos;
    } while (pos < ordered.size() && ordered[pos].variable == variable);
    if (coefficient == 0) continue;
    if (!in_range(coefficient)) return std::nullopt;

    // Both operands fit the export range, so the products fit int64. Bound the sum
    // of magnitudes as well to cover the solver's intermediate sums in any order.
    const auto [lo, hi] = domains[std::to_underlying(variable)];
    const auto first = coefficient * lo;
    const auto last = coefficient * hi;
    const auto [low, high] = std::minmax(first, last);
    result.magnitude += std::max(-low, high);
    if (result.magnitude > limit) return std::nullopt;
    result.bounds.lo += low;
    result.bounds.hi += high;
    result.terms.push_back({coefficient, variable});
  }
  return result;
}

bool constant_row_holds(Rel relation, std::int64_t rhs) {
  switch (relation) {
    case Rel::le: return 0 <= rhs;
    case Rel::eq: return 0 == rhs;
    case Rel::ge: return 0 >= rhs;
  }
  std::unreachable();
}

void emit_row(std::string& out, std::span<const Term> terms, Rel relation, std::int64_t rhs,
              bool objective = false) {
  // Standard FlatZinc has int_lin_le and int_lin_eq. Reverse >= by negating the row.
  const std::int64_t sign = relation == Rel::ge ? -1 : 1;
  out += relation == Rel::eq ? "constraint int_lin_eq([" : "constraint int_lin_le([";
  for (std::size_t i = 0; i < terms.size(); ++i)
    std::format_to(std::back_inserter(out), "{}{}", i == 0 ? "" : ", ",
                   sign * terms[i].coefficient);
  if (objective) out += ", -1";
  out += "], [";
  for (std::size_t i = 0; i < terms.size(); ++i)
    std::format_to(std::back_inserter(out), "{}x{}", i == 0 ? "" : ", ",
                   std::to_underlying(terms[i].variable));
  if (objective) out += ", objective";
  std::format_to(std::back_inserter(out), "], {});\n", sign * rhs);
}

}  // namespace

std::expected<std::string, FlatZincError> write_flatzinc(const Model& model) {
  std::string out;
  const auto domains = model.domains();
  for (std::size_t i = 0; i < domains.size(); ++i) {
    const auto [lo, hi] = domains[i];
    if (!in_range(lo) || !in_range(hi)) return std::unexpected(FlatZincError::domain_out_of_range);
    std::format_to(std::back_inserter(out), "var {}..{}: x{} :: output_var;\n", lo, hi, i);
  }

  std::optional<Expression> objective;
  if (model.objective()) {
    objective = expression(*model.objective(), domains);
    if (!objective || objective->magnitude + std::max(-objective->bounds.lo, objective->bounds.hi) > limit)
      return std::unexpected(FlatZincError::objective_out_of_range);
    const auto [lo, hi] = objective->bounds;
    std::format_to(std::back_inserter(out), "var {}..{}: objective;\n", lo, hi);
  }

  for (const auto& row : model.rows()) {
    const auto expr = expression(row.terms, domains);
    if (!expr) return std::unexpected(FlatZincError::row_out_of_range);
    if (expr->terms.empty()) {
      if (!constant_row_holds(row.relation, row.rhs)) out += "constraint int_eq(0, 1);\n";
      continue;
    }
    if (!in_range(row.rhs) || expr->magnitude + std::max(-row.rhs, row.rhs) > limit)
      return std::unexpected(FlatZincError::row_out_of_range);
    emit_row(out, expr->terms, row.relation, row.rhs);
  }

  if (objective) {
    if (!objective->terms.empty()) emit_row(out, objective->terms, Rel::eq, 0, true);
    out += "solve minimize objective;\n";
  } else {
    out += "solve satisfy;\n";
  }
  return out;
}

}  // namespace ycxx::cm
