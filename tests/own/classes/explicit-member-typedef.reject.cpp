// [dcl.fct.spec]/1,3, [dcl.typedef]/1
struct C {
  explicit typedef bool Alias; //! error
};
