// [class.mem.general]: replay diagnostics use the default initializer's source.
struct Record {
  int value = missing_initializer_name(); //! error
  Record() noexcept;
};

Record::Record() noexcept {}

struct Body {
  void broken() { missing_body_name(); } //! error
};
