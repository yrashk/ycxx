// [basic.lookup.unqual]: the class member blocks ordinary lookup of the outer type.
struct Entry {};
struct Box {
  enum Mode { Entry };
  void use() { Entry value; } //! error
};
