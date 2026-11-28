// [expr.type.conv]/1, [temp.dep.expr] (#286): a dependent class operand in a function
// template body copies when the body is instantiated.
int copies = 0;

struct Counted {
    int value;
    Counted(int v) : value(v) {}
    Counted(const Counted& other) : value(other.value) { ++copies; }
};

template<class T>
T copy_parens(const T& value) { return auto(value); }

template<class T>
T copy_braces(T& value) { return auto{value}; }

template<class T>
T fresh(int v) { return auto(T(v)); }

template<class T>
bool distinct(T& value) {
    auto copy = auto(value);
    copy.value += 1;
    return copy.value == value.value + 1;
}

int main() {
    Counted c(5);
    if (copy_parens(c).value != 5 || copies != 1)
        return 1;
    if (copy_braces(c).value != 5 || copies != 2)
        return 2;
    if (fresh<Counted>(6).value != 6 || copies != 2)
        return 3;
    if (!distinct(c) || c.value != 5 || copies != 3)
        return 4;
    return 0;
}
