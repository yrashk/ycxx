// #337 (ι, review): a declaration in a condition has an initializer ([stmt.pre]/1).
int f() { if (int x) {} return 0; } //! error
int main() { return f(); }
