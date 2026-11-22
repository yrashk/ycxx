// [class.access.general], [class.copy.assign], [temp.inst], [temp.constr.decl]

template<class T> requires true
struct Box {
  Box& operator=(const Box&);
  int field;
};

int assign(Box<int>& target, const Box<int>& source) {
  target = source;
  return target.field;
}
