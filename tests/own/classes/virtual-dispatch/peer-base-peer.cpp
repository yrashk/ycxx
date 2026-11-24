// Peer of peer-base.run.cpp, compiled by Clang.

extern int trace;

struct Base {
  int scale;
  Base(int s);
  virtual ~Base();
  virtual int key();
  virtual int twice();
  virtual int label() { return 7; }
  int seen_in_ctor = 0;
};

Base::Base(int s) : scale(s) { seen_in_ctor = label(); }
Base::~Base() { trace = trace * 10 + 1; }
int Base::key() { return scale; }
int Base::twice() { return 2 * key() % 10; }

int drive(Base& b) { return b.key() * 100 + b.twice() * 10 + b.label(); }
