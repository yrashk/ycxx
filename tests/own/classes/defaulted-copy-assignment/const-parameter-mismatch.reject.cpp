// GCC also locates the implicitly deleted definition. Put the declaration and use on the same marked line.
// [class.copy.assign], [dcl.fct.def.default]
struct Member {
    Member& operator=(Member&) = default;
};

struct Item { Member value; Item& operator=(const Item&) = default; }; void assign(Item& destination, Item& source) { destination = source; //! error }
