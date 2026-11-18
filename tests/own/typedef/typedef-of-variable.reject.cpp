// #133: a typedef-name may not take the name of a variable of its scope ([basic.scope.scope]).
int v;
typedef int v; //! error
