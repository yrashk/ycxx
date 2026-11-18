// #133: redeclarations of an array may differ in a bound only ([basic.link]/11).
extern int a[];
extern long a[]; //! error
