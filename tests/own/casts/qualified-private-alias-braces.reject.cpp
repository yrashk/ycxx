class S {
  using T = int;
};

int outside() {
  return S::T{7}; //! error
}
