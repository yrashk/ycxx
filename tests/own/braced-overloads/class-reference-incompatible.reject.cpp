// #503: a mutable lvalue reference can bind a named object, but
// cannot bind the temporary generated for a braced list ([dcl.init.list], [dcl.init.ref]).
struct Record {
  int first;
  int second;
};

void read(const Record&);
void write(Record&);

void check() {
  Record named{1, 2};
  write(named);
  read({1, 2});
  write({1, 2}); //! error
}
