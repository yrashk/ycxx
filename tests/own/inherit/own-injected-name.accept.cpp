// [class.pre], [class.member.lookup]
struct Parent {
    using Child = int;
};

struct Child : Parent {
    void select() {
        ::Child* exact = this;
        Child* selected = exact;
        exact = selected;
    }
};
