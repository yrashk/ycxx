// #133: `typedef` combines with no storage-class or constexpr specifier ([dcl.typedef]/1).
typedef static int X; //! error
