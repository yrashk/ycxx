// #133: the string literal and its null must fit the array ([dcl.init.string]/2).
char a[2] = "ab"; //! error
