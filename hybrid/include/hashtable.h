#pragma once
#include <stddef.h>
#include <stdint.h>
#include <omp.h>
#include "str_pool.h"

typedef struct HashEntry HashEntry;
typedef struct HashTable HashTable;

struct HashEntry {
	uint64_t hash;
	size_t sp_offset;						// offset into the string pool
	size_t count;
	size_t len;
};

struct HashTable {
	HashEntry* entries;
	StringPool* sp;
	omp_lock_t* locks;
	size_t n_reference;
	size_t n_locks;
	size_t stripe_size;

	size_t capacity;
	size_t size;					// Total number of used sp_offsets
	
	size_t total;					// Summation of all sp_offset entries
	size_t true_coll;			// Number of TRUE COLLISIONS: Multiple strings with the same 64-bit hashes
};

void ht_free(HashTable* ht);
int ht_init(HashTable* ht, size_t ht_capacity, size_t sp_capacity, size_t stripe_size);
HashTable* ht_new(size_t ht_capacity, size_t sp_capacity, size_t stripe_size);

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
int ht_insert(HashTable* ht, uint64_t hash, const char* str, size_t len, size_t count);
int ht_locked_insert(HashTable* ht, uint64_t hash, const char* str, size_t len, size_t count);

void ht_clear(HashTable* ht);

void ht_print(HashTable* ht);
