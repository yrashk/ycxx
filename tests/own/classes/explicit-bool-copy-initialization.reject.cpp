// [class.conv.fct]/4, [over.match.conv]/1
struct ForwardOrder {
  int code;
  constexpr explicit operator bool() const noexcept { return code == 7; }
};

struct ReverseOrder {
  int code;
  explicit constexpr operator bool() const noexcept { return code == 7; }
};

bool forward = ForwardOrder{7}; //! error
bool reverse = ReverseOrder{7}; //! error
