// [dcl.type.elab] requires a qualified elaborated name to name an existing type.
namespace display {}
struct Window {
  struct display::Frame* frame; //! error
};
