#pragma once
#include <stddef.h>

typedef struct StringPool StringPool;

struct StringPool {
	char* store;
	size_t used;
	size_t count;
	size_t capacity;
	omp_lock_t lock;
};

void 				sp_free(StringPool* pool);
StringPool* sp_new(size_t capacity, int concurrent);

// Adds a string to the string pool
// returns pointer to string or NULL on failure (not enough space to store string)
size_t 		  sp_add(StringPool* sp, const char* str, size_t len);

char*				sp_get(StringPool* sp, size_t offset);
void 				sp_clear(StringPool* sp);

void 				sp_print(StringPool* sp);
