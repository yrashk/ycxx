// #807: a lambda's noexcept-specifier is a contextually converted constant expression of type
// bool, which may not narrow ([except.spec]/2).
int main() {
  auto l = []() noexcept(2) { return 0; };  //! error
  return l();
}
