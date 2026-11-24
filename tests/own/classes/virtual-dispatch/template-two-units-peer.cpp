// The other unit of template-two-units.run.cpp: the same templates.
template <class T> struct Base {
  Base() {}
  virtual ~Base() { value = 0; }
  virtual T get() const { return value; }
  virtual int out() const;
  T value = 3;
};
template <class T> int Base<T>::out() const { return 100; }

template <class T> struct Derived : Base<T> {
  Derived() {}
  ~Derived() {}
  T get() const override { return this->value + T(10); }
};

int peer_get(const Base<int>& b) { return b.get(); }
int peer_out(const Base<int>& b) { return b.out(); }
Base<int>& peer_base() {
  static Base<int> b;
  return b;
}
Base<int>& peer_derived() {
  static Derived<int> d;
  return d;
}
