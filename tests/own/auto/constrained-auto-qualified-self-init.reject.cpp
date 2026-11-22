// Qualified lookup also finds the declaration whose type the initializer deduces.
template<class T> concept C = true;
namespace Inner {
  C auto x = Inner::x; //! error
}
