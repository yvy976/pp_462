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
	size_t len;
};

struct HashTable {
	HashEntry* entries;
	size_t capacity;
	size_t size;					// Total number of used keys
	size_t total;					// Summation of all key entries
	size_t true_coll;			// Number of TRUE COLLISIONS: Multiple strings with the same 64-bit hashes
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
int ht_insert(HashTable* ht, uint64_t hash, size_t key, size_t len, StringPool* str_pool);
int ht_locked_insert(HashTable* ht, uint64_t hash, size_t key, size_t len, StringPool* str_pool, omp_lock_t* locks, size_t stripe_size);

void ht_clear(HashTable* ht);

void ht_print(HashTable* ht);
