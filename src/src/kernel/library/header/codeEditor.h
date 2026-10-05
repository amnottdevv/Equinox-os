#ifndef CODEEDITOR_H
#define CODEEDITOR_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Open a file in the editor (create a new one if it does not exist)
void editor_open(const char* filename);

/* 0.4 Beta (eqgu): a hook run RIGHT AFTER a successful save.
 *   fn(fullpath, msg, msgsz) ->
 *      < 0  : no extra behavior — the plain "Saved." message (default)
 *      == 0 : OK     — the editor shows `msg` (e.g. "Cek: OK") green
 *      == 1 : FAILED — the editor shows `msg` red
 * NULL (the default) disables the hook — plain `edit` never changes.
 * The hook must not touch the editor's own buffers. */
void editor_set_check_fn(int (*fn)(const char*, char*, int));

#ifdef __cplusplus
}
#endif

#endif