// [class.access.general], [class.copy.assign], [temp.inst], [temp.constr.decl]

template<class T> requires true
class Box {
  Box& operator=(const Box&);

public:
  void assign_from(const Box& source) {
    *this = source;
  }
};

void assign(Box<int>& target, const Box<int>& source) {
  target.assign_from(source);
}
