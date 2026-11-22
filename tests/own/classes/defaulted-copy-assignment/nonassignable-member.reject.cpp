// GCC also locates the implicitly deleted definition. Put the declaration and use on the same marked line.
// [class.copy.assign], [dcl.fct.def.default]
struct Member {
    Member& operator=(const Member&) = delete;
};

struct Containing { Member value; Containing& operator=(const Containing&) = default; }; void assign(Containing& destination, const Containing& source) { destination = source; //! error }
