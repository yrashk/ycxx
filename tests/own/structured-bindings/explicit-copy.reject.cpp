// [dcl.struct.bind], [dcl.init.general]: copy-initialization excludes an explicit constructor.
struct ExplicitCopy {
  int value;
  explicit ExplicitCopy(const ExplicitCopy& source) : value(source.value) {}
};

void bind(ExplicitCopy& source) {
  auto [value] = source; //! error
}
