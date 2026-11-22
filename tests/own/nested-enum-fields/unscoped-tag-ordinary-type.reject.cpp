// [basic.lookup.general]: a hidden enum is not an ordinary type name.
struct Box { enum State { State }; };
Box::State value; //! error
