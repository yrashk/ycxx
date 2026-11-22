// #203: this slice requires a written destructor for nontrivial variant members.
// XFAIL: clang Clang supports this template's declaration without a written destructor.
// XFAIL: gcc GCC supports this template's declaration without a written destructor.
struct Resource { ~Resource() noexcept {} };
template<class Owner> union Storage { //! error
  Owner owner;
  int fallback;
};
static_assert(sizeof(Storage<Resource>) >= sizeof(Resource));
