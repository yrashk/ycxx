// [class.copy.assign], [dcl.fct.def.delete], [over.match.funcs], [temp.inst]
template<class Tag>
struct Ledger {
    Ledger& operator=(const Ledger&) & = delete;
    Ledger& operator=(const Ledger&) && = delete;
};

Ledger<int> integer_ledger;
Ledger<char> character_ledger;
