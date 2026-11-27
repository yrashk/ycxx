// [class.conv.fct]/1, [dcl.decl.general]/7, [dcl.fct.def.general]/1
struct Flag {
  explicit constexpr operator bool() const extra { return true; } //! error
};
