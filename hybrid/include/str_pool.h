#pragma once
#include <stddef.h>

typedef struct StringPool StringPool;

struct StringPool {
	char* store;
	size_t used;
	size_t count;
	size_t capacity;
};

void 				sp_free(StringPool* pool);
StringPool* sp_new(size_t capacity);

// Adds a string to the string pool
// returns pointer to string or NULL on failure (not enough space to store string)
char* 		  sp_add(StringPool* sp, const char* str, size_t len);
