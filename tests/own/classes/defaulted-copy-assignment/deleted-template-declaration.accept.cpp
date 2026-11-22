// [class.copy.assign], [dcl.fct.def.delete], [temp.inst]
template<class Tag>
struct Budget {
    Budget& operator=(const Budget&) = delete;
};

Budget<int> budget;

template<class Tag, unsigned Limit> requires (Limit > 0)
struct BoundedBudget {
    BoundedBudget& operator=(const BoundedBudget&) = delete;
};

BoundedBudget<int, 2> bounded;

template<class Tag>
struct AssignableBudget {
    AssignableBudget& operator=(const AssignableBudget&) { return *this; }
};

void assign(AssignableBudget<int>& left, const AssignableBudget<int>& right) {
    left = right;
}
