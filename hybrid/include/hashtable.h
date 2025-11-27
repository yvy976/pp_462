#pragma once
#include <stddef.h>
#include <stdint.h>
#include "str_pool.h"

typedef struct HashEntry HashEntry;
typedef struct HashTable HashTable;

struct HashEntry {
	uint64_t hash;
	size_t key;						// offset into the string pool
	size_t count;
};

struct HashTable {
	HashEntry* entries;
	size_t capacity;
	size_t size;					// Total number of used keys
	size_t total;					// Summation of all key entries
};

void ht_free(HashTable* ht);
HashTable* ht_new(size_t capacity);

// Takes the hash and string.
// The hash doesn't need to be modulo'd, that
// will be done in this function.
//
// Returns 1 on failure, 0 on success
//
// Linear probing to handle collisions.
//
// The hash is used to do true comparisons of the entries as well as
// partially used to index the table.
int ht_insert(HashTable* ht, uint64_t hash, size_t val, StringPool* str_pool);
