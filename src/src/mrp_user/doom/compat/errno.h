/* compat/errno.h — the RAMFS syscall layer reports its own negative
 * errno codes; these POSIX-ish values only satisfy code that inspects
 * errno after a failed stdio call. */
#ifndef MORPH_COMPAT_ERRNO_H
#define MORPH_COMPAT_ERRNO_H
#define EISDIR 21
#define EPERM  1
#define ENOENT 2
#define EINTR  4
#define EIO    5
#define EBADF  9
#define ENOMEM 12
#define EACCES 13
#define EEXIST 17
#define EINVAL 22
#define EMFILE 24
#define EPIPE  32
#define EDOM   33
#define ERANGE 34
extern int errno;
#endif
