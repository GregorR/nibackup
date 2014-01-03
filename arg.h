#ifndef ARG_H
#define ARG_H

#define ARGC(s, l) (!strcmp(arg, "-" #s) || !strcmp(arg, "--" #l))
#define ARG(s, l)  if ARGC(s, l)
#define ARGN(s, l) if (argv[argi+1] && ARGC(s, l))

#endif
