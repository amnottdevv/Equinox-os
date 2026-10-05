#ifndef MORPH_COMPAT_SIGNAL_H
#define MORPH_COMPAT_SIGNAL_H
#define SIGINT  2
#define SIGABRT 6
#define SIGSEGV 11
typedef void (*sighandler_t)(int);
#endif
