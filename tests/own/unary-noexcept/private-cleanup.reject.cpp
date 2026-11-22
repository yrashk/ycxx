// [expr.unary.noexcept], [conv.rval], [class.dtor]
class PrivateCleanup {
public:
  PrivateCleanup() noexcept;

private:
  ~PrivateCleanup() noexcept;
};

static_assert(noexcept(PrivateCleanup()));  //! error
