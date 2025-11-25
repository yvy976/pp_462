#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "str_pool.h"

/*
 struct StringPool {
	char* store;
	size_t used;
	size_t count;
	size_t capacity;
};
*/

void sp_free(StringPool* pool) {
	free(pool->store);
	free(pool);
}

StringPool* sp_new(size_t capacity) {
	StringPool* sp = malloc(sizeof(*sp));
	if (!sp) return NULL;

	sp->store = malloc(capacity);
	if (!sp->store) { free(sp); return NULL; }

	sp->store = 0;
	sp->count = 0;
	sp->capacity = capacity;

	return sp; 
}

// returns 1 on failure
// returns 0 on success
// reallocs the sp->store
static int _sp_resize(StringPool* sp, size_t new_capacity) {
	sp->store = realloc(sp->store, new_capacity);
	if (!sp->store) { return 1; }
	
	sp->capacity = new_capacity;

	return 0;
}

// Adds a string to the string pool
// returns pointer to string or NULL on failure (not enough space to store string)
// 'len' does NOT include the NULL terminator '\0'
char* 		  sp_add(StringPool* sp, const char* str, size_t len) {
	// Expand pool size if necessary
	if (sp->used + len >= sp->capacity) {
		size_t new_capacity = sp->capacity * 2;
		if (len >= new_capacity - sp->capacity) {
			fprintf(stderr, "[FATAL] irregularly large string len: %lu\n", len);
			return NULL;
		}
		_sp_resize(sp, new_capacity);
	}

	// copy in new string + NULL terminator
	char* str_start = sp->store + sp->used;
	memcpy(str_start, str, len);
	sp->store[sp->used + len] = '\0';

	// update stringpool fields
	sp->used += len + 1;
	sp->count += 1;

	return str_start;
}
