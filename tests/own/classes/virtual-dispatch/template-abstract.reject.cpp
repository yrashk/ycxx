// #1153 [class.abstract]: an object of an abstract class template specialization.
template <class T> struct Shape {
  virtual T area() = 0;
};
template <class T> struct Square : Shape<T> {
  T area() override { return 1; }
};
template <class T> struct Named : Shape<T> {
  virtual int name() { return 2; }
};
Square<int> fine;

void use() {
  Shape<int> s; //! error
  Named<int> n; //! error
}
