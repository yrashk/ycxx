// [over.match.funcs], [over.match.viable], [over.assign]
template<class Tag>
struct Ledger {
    Ledger& operator=(const Ledger&) && = delete;
};

void assign(Ledger<int>& destination, const Ledger<int>& source) {
    destination = source; //! error
}
