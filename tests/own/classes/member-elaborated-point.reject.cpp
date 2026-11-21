// [basic.scope.pdecl]
namespace graph {
Node* before; //! error

struct Edge {
  Node* early; //! error
  struct Node* from;
  Node* to;
};

Node* after;
}
