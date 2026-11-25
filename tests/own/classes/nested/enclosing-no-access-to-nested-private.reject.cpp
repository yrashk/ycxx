// #306 [class.access.nest]/1: the enclosing class has no special access to the members of a nested class.
class O {
public:
    class I {
        int hidden = 1;
    };
    int f(I& i) { return i.hidden; }  //! error
};
