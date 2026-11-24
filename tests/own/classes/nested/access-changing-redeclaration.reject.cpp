// #1216 [class.access.spec]/1,3: a redeclared member class keeps its original access.
class Envelope {
    struct Stamp;
protected:
    struct Stamp {};  //! error
};
