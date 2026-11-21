#include <initializer_list>
int consume(std::initializer_list<int> l) { return l.size() != 0 || l.begin() != l.end(); }
int main() { std::initializer_list<int> l; return consume(l); }
