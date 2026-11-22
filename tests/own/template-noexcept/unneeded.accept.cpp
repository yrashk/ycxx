// #828: an exception specification that depends on a template's parameters is evaluated for a
// specialization only when it is needed ([except.spec]/13): an invalid condition of one that is
// never needed is no error.
struct Missing {};
struct Present { static constexpr bool ready = true; };

template<class T> struct Reader {
  int immediate() const noexcept { return 0; }
  int delayed() const noexcept(T::ready) { return 1; }
};
template<class T> void free_delayed() noexcept(T::ready) {}

Reader<Missing> unused;
Reader<Present> demanded;
static_assert(noexcept(unused.immediate()));
static_assert(noexcept(demanded.delayed()));
static_assert(noexcept(free_delayed<Present>()));
int main() { return unused.immediate(); }
