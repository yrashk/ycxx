// [dcl.init.list]: in-class scalar and array lists keep ordinary initializer diagnostics.

struct TooManyScalarElements {
  inline static int value{1, 2}; //! error
};

struct NarrowingArrayElement {
  inline static unsigned char value[1] = {256}; //! error
};
