// #1417 [class.prop], [special]: a trivially copyable class needs at least one eligible copy
// or move constructor or assignment operator. When all four are declared `= default` with
// unsatisfied constraints, none is eligible and the class is not trivially copyable; with
// the constraints satisfied it is.
// XFAIL: gcc GCC 16 reports a class with no eligible copy or move operation as trivially copyable, against [class.prop]/1.1 (Clang follows the draft).
// XFAIL: ycxx type-trait built-ins do not classify class template specializations yet (#218)
template<class T> concept Small = sizeof(T) <= 2;

template<class T>
struct AllConstrained {
  T v;
  AllConstrained() = default;
  AllConstrained(const AllConstrained&) requires Small<T> = default;
  AllConstrained(AllConstrained&&) requires Small<T> = default;
  AllConstrained& operator=(const AllConstrained&) requires Small<T> = default;
  AllConstrained& operator=(AllConstrained&&) requires Small<T> = default;
};

static_assert(__is_trivially_copyable(AllConstrained<char>));
static_assert(!__is_trivially_copyable(AllConstrained<int>));
static_assert(__is_trivially_constructible(AllConstrained<int>));
