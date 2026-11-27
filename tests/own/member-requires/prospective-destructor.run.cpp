// #1417 [class.dtor], [over.match.best.general], [temp.constr.order]: prospective
// destructors; at the end of the class definition overload resolution selects the
// destructor, preferring the more constrained satisfied one. Which one ran is observed
// through its side effects.
int destroyed = 0;

template<class T> concept Small = sizeof(T) <= 2;

template<class T>
struct Res {
  T v;
  ~Res() requires Small<T> = default;
  ~Res() { ++destroyed; }
};

template<class T>
struct Tagged {
  T v;
  ~Tagged() requires Small<T> { destroyed += 10; }
  ~Tagged() requires (!Small<T>) { destroyed += 100; }
};

void scope_small() { Res<char> r{'a'}; (void)r; }
void scope_big() { Res<long long> r{1}; (void)r; }
void tagged_small() { Tagged<short> t{1}; (void)t; }
void tagged_big() { Tagged<int> t{1}; (void)t; }

int main() {
  scope_small();
  if (destroyed != 0) return 1;
  scope_big();
  if (destroyed != 1) return 2;
  tagged_small();
  if (destroyed != 11) return 3;
  tagged_big();
  if (destroyed != 111) return 4;
  return 0;
}
