// #761 [dcl.type.elab], [basic.lookup.elab].
namespace lab { struct Instrument {}; }

enum lab::Instrument* instrument; //! error
