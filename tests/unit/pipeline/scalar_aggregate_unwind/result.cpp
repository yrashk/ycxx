#include "result.hpp"

Small small_result(int input) { return {result_field(input)}; }

Pair named_result(int input) {
  Pair result{result_field(input), 19};
  return result;
}

Wide wide_result(int input) { return {11, result_field(input), 37}; }

struct Nontrivial {
  int value;
  ~Nontrivial() noexcept;
};

Nontrivial nontrivial_result(int input) { return {input}; }
