// #738: a completed variant needs cleanup if union construction throws.
// REQUIRES: darwin
// PARITY: #648 Darwin still needs the native cleanup and caller frames.
// XFAIL: clang Clang supports exceptional cleanup of the active member.
// XFAIL: gcc GCC supports exceptional cleanup of the active member.
extern void external_throw();
struct Resource {
  int value;
  ~Resource() noexcept {}
};
union Storage {
  Resource owner;
  int fallback;
  Storage() : owner{7} { external_throw(); } //! error
  ~Storage() noexcept {}
};
void create() { Storage object; }
