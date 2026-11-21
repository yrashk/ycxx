// #503: a reference member can bind a temporary of its referred-to type.
// List-initializing the containing class still prohibits narrowing ([dcl.init.list]).
struct Record {
  const int& value;
};

void consume(const Record&);

void valid() { consume({3}); }
void invalid() { consume({3.5}); } //! error
void valid_nested() { consume({{3}}); }
void invalid_nested() { consume({{3.5}}); } //! error
