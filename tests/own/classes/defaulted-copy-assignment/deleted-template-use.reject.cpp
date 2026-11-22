// [class.copy.assign], [dcl.fct.def.delete], [temp.inst], [over.assign]
template<class Tag>
struct Budget {
    Budget& operator=(const Budget&) = delete;
};

void assign(Budget<int>& left, const Budget<int>& right) {
    left = right; //! error
}
