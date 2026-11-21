namespace example {
class exception {
public:
  inline virtual constexpr ~exception() noexcept {}
  virtual constexpr const char* what() const noexcept { return "exception"; }
};

class read_error : public exception {
public:
  const char* what() const noexcept override { return "read error"; }
};

static_assert(__has_virtual_destructor(exception));
static_assert(__has_virtual_destructor(read_error));
}
