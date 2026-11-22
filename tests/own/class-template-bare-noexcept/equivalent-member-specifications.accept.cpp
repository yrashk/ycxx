// [except.spec], [temp.inst], [temp.mem]
template<class T> struct Box {
    int first() const noexcept;
    int second() const noexcept(true);

    template<class U> int read(U value) const noexcept;
};

template<class T>
int Box<T>::first() const noexcept(true) { return 1; }

template<class T>
int Box<T>::second() const noexcept { return 2; }

template<class T> template<class U>
int Box<T>::read(U value) const noexcept { return value; }

int use(const Box<int>& box) {
    return box.first() + box.second() + box.read(3);
}
