// [dcl.init.aggr]: the first field's default runs before the later field is initialized.
struct Limits {
  int first = last;
  int last = 42;
};

static_assert(Limits{17}.first == 17);
static_assert(Limits{}.first == 42); //! error
