// #798 [temp.func.order], [temp.deduct.partial], [temp.deduct.type].
template<int>
struct Identity { using type = long; };

template<int I, class T>
int choose(typename Identity<I>::type*, T);
template<int I, class T>
long choose(T*, T);

void ambiguous() {
  long value = 0;
  choose<7>(&value, value); //! error
}
