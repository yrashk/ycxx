// [class.conv.fct]/1, [temp.constr.decl]/1, [dcl.fct.def.general]/1
struct Flag {
  constexpr explicit operator bool() const requires(false) { return true; } //! error
};
