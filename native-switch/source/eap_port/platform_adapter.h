// Adapter: maps EapRules platform.h to our environment
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// EapRules uses these types
typedef unsigned int uint;

// Array size macro
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

#ifdef __cplusplus
extern "C" {
#endif

// Game directory - we'll set this to our data path
void io_set_game_dir(const char *dir);
const char *io_game_dir(void);
const char *io_writable_dir(void);

// Path helper - matches EapRules fix_path.h signature
// Returns the fixed path (may be the same pointer or a temp buffer)
const char *fix_path(const char *path, char *buf, size_t buf_len);

#ifdef __cplusplus
}
#endif
