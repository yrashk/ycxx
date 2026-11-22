// The declaration's own name hides the outer variable during deduction.
template<class T> concept C = true;
constexpr int x = 1;
namespace Inner {
  C auto x = x; //! error
}
