// #203 [dcl.init.aggr]: the first variant's empty list is a copy-list initialization.
// XFAIL: gcc GCC 16.2 leaves the first variant uninitialized under the pinned C++26 rule.
struct Resource {
  explicit Resource() noexcept {}
  ~Resource() noexcept {}
};
union Choice {
  Resource owner;
  int fallback;
  ~Choice() noexcept {}
};
void construct() {
  Choice value{}; //! error
}
