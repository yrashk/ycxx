// [class.mem.general] requires the token 0 after expansion.
#define ZERO 00
struct Wrong {
    virtual int score(int) = ZERO;  //! error
};
