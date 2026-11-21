// [dcl.type.elab] requires the class-key to agree with the kind of declared type.
namespace storage {
union Cell;
enum Mode { idle };

struct Table {
  struct Cell* cell; //! error
  struct Mode* mode; //! error
  enum Missing* missing; //! error
};
}
