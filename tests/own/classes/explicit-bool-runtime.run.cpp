// [class.conv.fct]/4, [over.match.conv]/1, [expr.static.cast]/5
struct ForwardOrder {
  int code;
  constexpr explicit operator bool() const noexcept { return code == 7; }
};

struct ReverseOrder {
  int code;
  explicit constexpr operator bool() const noexcept { return code == 7; }
};

int main(int argc, char**) {
  bool first_is_good = argc <= 1;
  int first_code = first_is_good ? 7 : 9;
  int second_code = first_is_good ? 9 : 7;
  const ForwardOrder forward_first{first_code};
  const ForwardOrder forward_second{second_code};
  const ReverseOrder reverse_first{first_code};
  const ReverseOrder reverse_second{second_code};

  if (static_cast<bool>(forward_first) != first_is_good) return 1;
  if (static_cast<bool>(forward_second) == first_is_good) return 2;
  if (static_cast<bool>(reverse_first) != first_is_good) return 3;
  if (static_cast<bool>(reverse_second) == first_is_good) return 4;
  return 0;
}
