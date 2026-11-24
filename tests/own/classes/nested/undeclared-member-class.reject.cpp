// #306 [class.nest]/3, [class.mem]: O::I can only be defined if O declares a member class I.
class O { public: int x; };
class O::I { };  //! error
