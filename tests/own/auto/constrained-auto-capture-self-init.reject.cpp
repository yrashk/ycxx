// A lambda capture cannot name a local whose type is still undeduced.
template<class T> concept Any = true;
int main() {
  Any auto value = [&value] { return 0; }(); //! error
}
