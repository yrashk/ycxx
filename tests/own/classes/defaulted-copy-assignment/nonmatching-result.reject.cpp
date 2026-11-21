// [class.copy.assign], [dcl.fct.def.default]
struct Item {
    int value;
    void operator=(const Item&) = default; //! error
};
