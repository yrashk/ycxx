struct C {
    explicit C(int) {}
    C(long) {}
};

C ordinary_copy = 1;
C direct_list{1};
C copy_list = {1}; //! error
