// [class.access.general], [temp.inst], [temp.constr.decl]

template<class T> requires true
class Box {
  int field;
};

int read(const Box<int>& box) {
  return box.field;  //! error
}
