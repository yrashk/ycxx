// #199, [expr.const]: a reached reinterpret_cast of any form prevents constant evaluation,
// including one in a template specialization. reinterpret-glvalue-constexpr.reject.cpp has
// the glvalue form.
int item;
using Function = void (*)(int);
void function();

constexpr const char* bytes = reinterpret_cast<const char*>(&item); //! error
constexpr Function other_function = reinterpret_cast<Function>(&function); //! error

template <class T> constexpr bool reaches(const T* p) {
  return reinterpret_cast<const unsigned char*>(p) != nullptr; //! error[gcc]
}
static_assert(reaches(&item)); //! error
