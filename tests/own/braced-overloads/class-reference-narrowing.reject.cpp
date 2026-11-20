// #503: copy-list-initializing the temporary for a reference argument
// prohibits narrowing conversions in its member initializers ([dcl.init.list]).
struct Record {
  int first;
  int second;
};

int read(const Record&);

int valid() { return read({1, 2}); }
int invalid() { return read({1.25, 2}); } //! error
