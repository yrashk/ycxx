// #133: a variable may not take the name of a typedef-name of its scope ([basic.scope.scope]).
typedef int A;
int A; //! error
