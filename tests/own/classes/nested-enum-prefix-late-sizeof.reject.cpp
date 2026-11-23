struct Owner {
    enum Result { width = sizeof(member) }; //! error
    int member;
};
