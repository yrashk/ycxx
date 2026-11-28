// [new.delete.single]: __builtin_operator_new(args) and __builtin_operator_delete(args) call the
// replaceable global function that overload resolution selects for args (docs/EXTENSIONS.md).
// The peer replaces them and checks each call's arguments and order. No header declares them:
// the usual functions are implicitly declared ([basic.stc.dynamic.general]).
// PEER: builtin-operator-new-provider.cpp
using Count = decltype(sizeof(0));
namespace std { enum class align_val_t : Count {}; }

#if !__has_builtin(__builtin_operator_new) || !__has_builtin(__builtin_operator_delete)
#error the allocation builtins are unavailable
#endif

extern "C" int allocation_log();

void* p = __builtin_operator_new(4);
void* q = __builtin_operator_new(16, static_cast<std::align_val_t>(32));
void r() { __builtin_operator_delete(p); }

// A dependent call is resolved at instantiation; a pack expands into the arguments.
template <class T> T* make(Count n) {
  return static_cast<T*>(__builtin_operator_new(n * sizeof(T), std::align_val_t(alignof(T))));
}
template <class T> void drop(T* x, Count n) { __builtin_operator_delete(x, n * sizeof(T), std::align_val_t(alignof(T))); }
template <class... A> void* forward_new(A... a) { return __builtin_operator_new(a...); }
template <class... A> void forward_delete(A... a) { __builtin_operator_delete(a...); }

int main() {
  r();
  __builtin_operator_delete(q, static_cast<std::align_val_t>(32));
  long* l = make<long>(3);
  l[2] = 7;
  drop(l, 3);
  forward_delete(forward_new(Count(8)), Count(8));
  return allocation_log();
}
