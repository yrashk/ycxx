// [basic.lookup.argdep], [class.friend], [over.match.oper]
namespace hidden_binary {
struct Item {
    friend int operator+(const Item&, int) { return 1; }
};
}

int miss(const hidden_binary::Item& item) {
    return hidden_binary::operator+(item, 2);  //! error
}
