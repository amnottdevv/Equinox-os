#ifndef CODEEDITOR_H
#define CODEEDITOR_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Open a file in the editor (create a new one if it does not exist)
void editor_open(const char* filename);

#ifdef __cplusplus
}
#endif

#endif