struct Source {
    operator int() &&;
};

int convert(Source& source) {
    return static_cast<int>(source); //! error
}
