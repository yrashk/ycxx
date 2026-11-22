// [class.dtor]
struct Deleted { ~Deleted() = delete; };
void local() { Deleted object; }      //! error
void temporary() { (void)Deleted{}; }  //! error
