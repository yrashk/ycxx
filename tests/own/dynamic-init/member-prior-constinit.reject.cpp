int next();
struct Record { int first = next(); };
extern constinit Record one;
Record one; //! error
