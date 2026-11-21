// [stmt.ranged], [class.dtor]
struct Private { private: ~Private(); };
void range(Private (&items)[1]) {
    for (Private item : items) {}  //! error
}
