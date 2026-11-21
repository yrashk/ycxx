// [class.copy.assign], [dcl.fct.def.default]
class Item {
    int value;
    Item& operator=(const Item&) = default;

public:
    void set_from(const Item& source) {
        *this = source;
    }
};

void assign(Item& destination, const Item& source) {
    destination = source; //! error
}
