// [class.friend]/2, [class.access]: a friend defined in a class template may name the private
// members of the specialization that defines it; a function that is not a friend may not.
template <class T>
class Box {
    T value = 0;
    friend T peek(const Box& b) { return b.value; }
};

int other(const Box<int>& b) { return b.value; } //! error

int main() { return peek(Box<int>()) + other(Box<int>()); }
