// The x87 representation is needed for a Linux target even when the bootstrap
// host's long double has a different format.
export module ycxx.lex:extended_float;

import std;

namespace ycxx::lex::detail {

using Word = std::uint32_t;
using Wide = std::uint64_t;

class Significand {
public:
  explicit Significand(Word value = 0) {
    if (value) words_.push_back(value);
  }

  void multiply_add(Word factor, Word digit = 0) {
    Wide carry = digit;
    for (Word& word : words_) {
      carry += Wide(word) * factor;
      word = Word(carry);
      carry >>= 32;
    }
    if (carry) words_.push_back(Word(carry));
  }

  void multiply_by_power_of_five(std::size_t power) {
    constexpr Word five_to_thirteen = 1220703125;
    for (; power >= 13; power -= 13) multiply_add(five_to_thirteen);
    Word factor = 1;
    for (std::size_t i = 0; i < power; ++i) factor *= 5;
    multiply_add(factor);
  }

  void shift_left(std::size_t count) {
    if (words_.empty()) return;
    words_.insert(words_.begin(), count / 32, 0);
    const Word bits = count % 32;
    Wide carry = 0;
    for (Word& word : words_) {
      carry |= Wide(word) << bits;
      word = Word(carry);
      carry >>= 32;
    }
    if (carry) words_.push_back(Word(carry));
  }

  std::size_t bit_width() const {
    if (words_.empty()) return 0;
    return 32 * (words_.size() - 1) + std::bit_width(words_.back());
  }

  std::strong_ordering operator<=>(const Significand& other) const {
    if (const auto size = words_.size() <=> other.words_.size(); size != 0) return size;
    return std::lexicographical_compare_three_way(words_.rbegin(), words_.rend(), other.words_.rbegin(), other.words_.rend());
  }

  void subtract(const Significand& other) {
    Wide borrow = 0;
    for (std::size_t i = 0; i < words_.size(); ++i) {
      const Wide subtrahend = i < other.words_.size() ? other.words_[i] : 0;
      const Wide difference = Wide(words_[i]) - subtrahend - borrow;
      words_[i] = Word(difference);
      borrow = difference >> 63;
    }
    while (!words_.empty() && words_.back() == 0) words_.pop_back();
  }

private:
  std::vector<Word> words_;
};

std::int64_t literal_exponent(std::string_view spelling) {
  const bool negative = spelling.starts_with('-');
  if (negative || spelling.starts_with('+')) spelling.remove_prefix(1);
  constexpr std::int64_t limit = std::int64_t{1} << 40;
  std::int64_t value = 0;
  for (char digit : spelling) {
    value = value * 10 + digit - '0';
    if (value > limit) return negative ? -limit : limit;
  }
  return negative ? -value : value;
}

std::int64_t binary_order(const Significand& numerator, const Significand& denominator) {
  auto order = std::int64_t(numerator.bit_width()) - std::int64_t(denominator.bit_width());
  if (order < 0) {
    auto scaled = numerator;
    scaled.shift_left(std::size_t(-order));
    if (scaled < denominator) --order;
  } else {
    auto scaled = denominator;
    scaled.shift_left(std::size_t(order));
    if (numerator < scaled) --order;
  }
  return order;
}

}  // namespace ycxx::lex::detail

export namespace ycxx::lex {

// [lex.fcon]: round a decimal or hexadecimal literal to the nearest x87 number.
// A normal number stores its leading bit explicitly. A subnormal has the same
// scale as the least normal number, with that bit clear and exponent field zero.
std::array<std::uint64_t, 2> x87_literal(std::string_view integer, std::string_view fraction,
                                         std::string_view exponent, bool hexadecimal) {
  using detail::Significand;
  using detail::Wide;
  constexpr Wide integer_bit = Wide{1} << 63;
  constexpr int maximum_order = 16383;
  constexpr int minimum_order = -16382;
  constexpr int half_minimum_order = minimum_order - 64;
  constexpr std::array<Wide, 2> infinity{integer_bit, 0x7fff};
  const std::int64_t digit_scale = hexadecimal ? 4 : 1;
  const std::string digits = std::string(integer) + std::string(fraction);
  const auto first = digits.find_first_not_of('0');
  if (first == digits.npos) return {};
  const auto last = digits.find_last_not_of('0');
  std::string_view significant = std::string_view(digits).substr(first, last - first + 1);
  auto scale = detail::literal_exponent(exponent) + digit_scale *
      (std::int64_t(digits.size() - last - 1) - std::int64_t(fraction.size()));
  const auto magnitude = scale + digit_scale * std::int64_t(significant.size() - 1);
  if (magnitude > (hexadecimal ? maximum_order : 4932)) return infinity;
  if (magnitude < (hexadecimal ? half_minimum_order - 3 : -4951)) return {};

  // An x87 rounding midpoint is a binary rational, with denominator at most
  // 2^16446. Its decimal representation has fewer than 20,000 significant
  // digits. Extra digits can only move an exact midpoint upwards.
  const std::size_t retained = hexadecimal ? 32 : 20000;
  const bool discarded = significant.size() > retained;
  if (discarded) {
    scale += digit_scale * std::int64_t(significant.size() - retained);
    significant = significant.substr(0, retained);
  }
  Significand numerator;
  Significand denominator(1);
  for (char digit : significant) {
    const unsigned n = digit <= '9' ? digit - '0' : 10 + ((digit | 0x20) - 'a');
    numerator.multiply_add(hexadecimal ? 16 : 10, n);
  }
  if (!hexadecimal && scale > 0) numerator.multiply_by_power_of_five(std::size_t(scale));
  if (!hexadecimal && scale < 0) denominator.multiply_by_power_of_five(std::size_t(-scale));
  auto order = detail::binary_order(numerator, denominator) + scale;
  if (order > maximum_order) return infinity;
  if (order < half_minimum_order) return {};
  const auto normalized = std::max(order, std::int64_t(minimum_order));
  const auto shift = scale + 63 - normalized;
  if (shift < 0) denominator.shift_left(std::size_t(-shift));
  else numerator.shift_left(std::size_t(shift));

  Wide bits = 0;
  for (int bit = 63; bit >= 0; --bit) {
    auto divisor = denominator;
    divisor.shift_left(std::size_t(bit));
    if (numerator < divisor) continue;
    numerator.subtract(divisor);
    bits |= Wide{1} << bit;
  }
  numerator.shift_left(1);
  const auto halfway = numerator <=> denominator;
  if (halfway > 0 || (halfway == 0 && ((bits & 1) || discarded))) {
    ++bits;
    if (bits == 0) {
      bits = integer_bit;
      if (++order > maximum_order) return infinity;
    }
  }
  const Wide biased = (bits & integer_bit) ? Wide(std::max(order, std::int64_t(minimum_order)) + 16383) : 0;
  return {bits, biased};
}

}  // namespace ycxx::lex
