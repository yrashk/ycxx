struct Owner {
    enum class Result : decltype(+member) { value = 7 }; //! error
    unsigned char member;
};
