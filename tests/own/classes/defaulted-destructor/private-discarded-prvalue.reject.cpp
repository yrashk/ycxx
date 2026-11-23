// #157 [expr.static.cast], [expr.context], [conv.rval], [class.dtor], [dcl.type.decltype].
struct HiddenResult {
private:
  ~HiddenResult() = default;
};

HiddenResult make();

using ResultType = decltype(make());

void discard_result() {
  static_cast<void>(make()); //! error
}

using NestedDiscard = decltype(static_cast<void>(make())); //! error
