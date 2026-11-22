// [class.access.general], [class.copy.assign], [temp.inst], [temp.constr.decl]

template<class T> requires true
class Box {
  Box& operator=(const Box&);
};

void assign(Box<int>& target, const Box<int>& source) {
  target = source;  //! error
}
