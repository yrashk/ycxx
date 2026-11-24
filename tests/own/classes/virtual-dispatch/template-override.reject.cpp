// #1153 [class.virtual]: what a class template's member overrides is checked where its base is
// known: in the definition without a dependent base, and in each specialization otherwise.
struct Base {
  virtual ~Base() {}
  virtual int f() { return 1; }
  virtual int g() noexcept { return 2; }
  virtual int h() final { return 3; }
};
struct Other {
  virtual ~Other() {}
};

template <class T> struct NoBase {
  int f() override { return 4; } //! error
};
NoBase<int> no_base;

template <class T> struct Overrides : T {
  int f() override { return 5; } //! error
};
Overrides<Base> fine;
Overrides<Other> no_f;

template <class T> struct Returns : Base {
  T f() override { return 6; } //! error
};
Returns<int> same;
Returns<long> different;

template <class T> struct Throws : T {
  int g() { return 7; } //! error
};
Throws<Base> throws;

template <class T> struct Final : T {
  int h() { return 8; } //! error
};
Final<Base> overrides_final;

template <class T> struct NotVirtual {
  int f() final { return 9; } //! error
};
NotVirtual<int> not_virtual;
