// [class.copy.assign], [dcl.fct.def.default]
struct Scalar {
    const int value;
    Scalar& operator=(const Scalar&) = default;
};

struct Reference {
    int& value;
    Reference& operator=(const Reference&) = default;
};

struct Member {
    Member& operator=(const Member&) = delete;
};

struct Containing {
    Member value;
    Containing& operator=(const Containing&) = default;
};
