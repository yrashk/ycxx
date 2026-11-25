// [class.copy.assign], [dcl.fct.def.delete], [over.match.funcs], [over.assign]
template<class Tag>
struct Ledger {
    Ledger& operator=(const Ledger&) & = delete;
    Ledger& operator=(const Ledger&) && = delete;
};

void assign(Ledger<int>& destination, const Ledger<int>& source) {
    destination = source; //! error
}
