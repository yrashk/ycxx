#pragma once

extern int observed;

template<class T> struct Cell {
  ~Cell() noexcept { observed = observed * 10 + 2; }
};

// [temp.expl.spec], [dcl.inline]: the specialization has its own inline declarations.
#if defined(DECLARED_INLINE)
template<> inline Cell<int>::~Cell() noexcept;
#endif

template<>
#if defined(WRITTEN_INLINE)
inline
#endif
Cell<int>::~Cell() noexcept {
  observed = observed * 10 + 7;
}
