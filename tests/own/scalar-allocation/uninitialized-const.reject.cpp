// [dcl.init.general]: const scalar objects need an initializer.
int main() { auto p = new const int; delete p; } //! error
