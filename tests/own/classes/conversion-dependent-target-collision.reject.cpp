// #339: the declarations conflict only when substitution makes their
// targets identical ([class.conv.fct], [temp.inst]).
template<class T>
struct Converter {
  using value_type = T;
  operator int() const;
  operator value_type() const; //! error
};

static_assert(sizeof(Converter<long>) > 0);
static_assert(sizeof(Converter<int>) > 0);
