// ζ c2 (#88): constructors and destructors of a class template run in RAII order: locals are
// destroyed in reverse order of construction at the end of their block, observed through a log
// held by reference; the destructor is declared with the injected-class-name, in the class and
// outside it as Tagged<N>::~Tagged ([temp.class], [temp.local], [class.dtor], [stmt.dcl],
// [class.base.init]).
// EXIT: 0
struct Log {
  int seq;
  int count;
  void push(int x) { seq = seq * 10 + x; ++count; }
};

template<class T>
struct Guard {
  Log& log;
  T id;
  Guard(Log& l, T i) : log(l), id(i) { log.push(id); }
  ~Guard() { log.push(id + 4); }
};

template<int N>
struct Tagged {
  Log& log;
  Tagged(Log& l);
  ~Tagged();
};

template<int N>
Tagged<N>::Tagged(Log& l) : log(l) { log.push(N); }

template<int N>
Tagged<N>::~Tagged() { log.push(N + 5); }

int main() {
  Log log{0, 0};
  {
    Guard<int> a(log, 1);
    Guard<long> b(log, 2L);
    if (log.seq != 12) return 1;
  }
  if (log.seq != 1265) return 2;
  if (log.count != 4) return 3;
  Log t{0, 0};
  {
    Tagged<1> x(t);
    {
      Tagged<2> y(t);
    }
    if (t.seq != 127) return 4;
  }
  if (t.seq != 1276) return 5;
  return 0;
}
