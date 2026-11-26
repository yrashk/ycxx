// [dcl.decl.general], [dcl.fct.def.general]/1
struct Record {
  Record& operator=(Record&&) unexpected = delete; //! error
};
