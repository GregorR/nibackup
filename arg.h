#ifndef ARG_H
#define ARG_H

#define ARGLC(l)    (!strcmp(arg, "--" #l))
#define ARGC(s, l)  (!strcmp(arg, "-" #s) || ARGLC(l))
#define ARGL(l)     if ARGLC(l)
#define ARG(s, l)   if ARGC(s, l)
#define ARGLN(l)    if (argv[argi+1] && ARGLC(l))
#define ARGN(s, l)  if (argv[argi+1] && ARGC(s, l))

#endif
