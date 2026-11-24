// Deduction can see a base once the argument class is complete
// ([temp.deduct.call]/4.3, #1144).
template<class T> struct Box {};

template<class T> T select(const Box<T>*);
void select(...);

struct Forward;
extern Forward* pointer;
static_assert(__is_same(decltype(select(pointer)), void));

struct Forward : Box<int> {};
static_assert(__is_same(decltype(select(pointer)), int));

template<class T> T mutable_box(Box<T>*);
void mutable_box(...);
extern const Forward* const_pointer;
static_assert(__is_same(decltype(mutable_box(const_pointer)), void));
