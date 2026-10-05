#ifndef FILEMANAGER_H
#define FILEMANAGER_H

#ifdef __cplusplus
extern "C" {
#endif

// Open the file manager at a given path (NULL = root)
void filemanager_open(const char* start_path);

#ifdef __cplusplus
}
#endif

#endif