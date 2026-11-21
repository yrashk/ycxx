namespace protocol {
enum { repeated = 1 };
extern "C" {
enum { repeated = 2 }; //! error
}
}
