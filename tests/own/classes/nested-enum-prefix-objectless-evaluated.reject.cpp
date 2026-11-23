struct Owner {
    int member;
    enum Result { value = member }; //! error
};
