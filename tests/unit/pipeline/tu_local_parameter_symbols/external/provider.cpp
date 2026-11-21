namespace shared { struct Tag { int key; }; }
namespace { using LocalSpelling = shared::Tag; }

int choose(const LocalSpelling& tag) { return 40 + tag.key; }
