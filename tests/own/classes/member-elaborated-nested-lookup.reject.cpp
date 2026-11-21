// [basic.scope.scope] places the introduced declaration in namespace scope.
namespace transport {
struct Station {
  struct Route* route;
};

Route* namespace_route;
Station::Route* class_route; //! error
}

Route* global_route; //! error
