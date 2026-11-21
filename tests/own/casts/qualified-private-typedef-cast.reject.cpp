class S {
  typedef int T;
};

int outside() {
  return S::T(7); //! error
}
