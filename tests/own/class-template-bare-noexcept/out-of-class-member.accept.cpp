// [except.spec], [temp.inst], [expr.unary.noexcept]
template<class T> struct Box {
    int get() const noexcept;
};

template<class T> int Box<T>::get() const noexcept { return 1; }

extern Box<int> box;
static_assert(noexcept(box.get()));
