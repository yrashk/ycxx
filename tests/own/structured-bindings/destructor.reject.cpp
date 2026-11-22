// [dcl.struct.bind], [class.dtor]: the hidden object needs an accessible, non-deleted destructor.
struct Private {
  int member;
private:
  ~Private();
};

struct Deleted {
  int member;
  ~Deleted() = delete;
};

Private make_private();
Deleted make_deleted();

void copy(Private& source) {
  auto [member] = source; //! error
}

void temporary() {
  const auto& [member] = make_private(); //! error
}

void deleted_destructor() {
  auto&& [member] = make_deleted(); //! error
}

int alias(Private& source, Deleted& other) {
  auto& [member] = source;
  auto& [second] = other;
  return member + second;
}
