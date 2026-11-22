// [basic.lookup.general]
struct Holder {
    int Entry;
    struct Entry* source;
    Entry* use; //! error
};
