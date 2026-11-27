// [class.conv.fct]/4, [over.match.conv]/1, [expr.static.cast]/5
struct Flag {
  constexpr explicit operator bool() const { return true; }
  explicit operator int&() const;
};

constexpr Flag constant{};
static_assert(static_cast<bool>(constant));

bool to_bool(const Flag& value) {
  return static_cast<bool>(value);
}

int main() {
  const Flag runtime{};
  return to_bool(runtime) ? 0 : 1;
}
