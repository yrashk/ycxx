// ζ c2 (#88): specializations with different arguments are distinct types, so ordinary
// functions overload on Box<int>& versus Box<long>& (and Arr<3>& versus Arr<4>&), and overload
// resolution picks the exact match even where the argument types would convert
// ([temp.type], [temp.class], [over.match.best]).
// EXIT: 0
template<class T>
struct Box {
  T v;
};

template<int N>
struct Arr {
  int data[N];
};

int which(Box<int>&) { return 1; }
int which(Box<long>&) { return 2; }
int which(Box<char>&) { return 3; }
int which(const Box<int>&) { return 4; }

int len(Arr<3>&) { return 3; }
int len(Arr<4>&) { return 4; }

int main() {
  Box<int> i{0};
  Box<long> l{0L};
  Box<char> c{'c'};
  const Box<int> ci{0};
  if (which(i) != 1) return 1;
  if (which(l) != 2) return 2;
  if (which(c) != 3) return 3;
  if (which(ci) != 4) return 4;
  Arr<3> a3{};
  Arr<4> a4{};
  if (len(a3) != 3) return 5;
  if (len(a4) != 4) return 6;
  Box<int>* pi = &i;
  Box<long>* pl = &l;
  if (which(*pi) != 1) return 7;
  if (which(*pl) != 2) return 8;
  return 0;
}
