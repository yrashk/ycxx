// [class.access.spec]: an access label does not take an attribute-specifier-seq.
class Reading {
    [[maybe_unused]] public:  //! error
    int data;
};
