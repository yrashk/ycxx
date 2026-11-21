// [class.copy.assign], [dcl.fct.def.default]
struct Item {
    int value;
    Item& operator=(Item&) = default;
};

Item& assign(Item& destination, Item& source) {
    return destination = source;
}
