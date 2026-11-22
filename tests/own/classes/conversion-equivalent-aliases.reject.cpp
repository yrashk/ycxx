// #339: two aliases of the same type identify the same conversion
// target ([dcl.typedef], [class.conv.fct], [class.mem.general]).
struct Converter {
  using first_type = int;
  using second_type = first_type;
  operator first_type() const;
  operator second_type() const; //! error
};
