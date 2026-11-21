struct Source {
    operator int() & noexcept;
    operator int() &; //! error
};
