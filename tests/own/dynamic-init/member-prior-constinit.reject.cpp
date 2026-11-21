int next();
struct Record;
extern constinit Record one;
struct Record { int first = next(); }; Record one; //! error
