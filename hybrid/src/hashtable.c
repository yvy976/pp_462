#include <stdlib.h>
#include <string.h>
#include <stdio.h> 			// DEBUGGING
#include <omp.h>
#include "hashtable.h"

/*
struct HashEntry {
	uint64_t hash;
	size_t key;
	size_t len;
	size_t count;
};

struct HashTable {
	HashEntry* entries;
	size_t capacity;
	size_t size;
	size_t total;
	size_t true_collisions;
};
*/

void ht_free(HashTable* ht) {
	free(ht->entries);
	free(ht);
}

HashTable* ht_new(size_t capacity) {
	HashTable* ht = malloc(sizeof(*ht));
	if (!ht) return NULL;

	ht->entries = calloc(capacity, sizeof(HashEntry));
	if (!ht->entries) { free(ht); return NULL; }

	ht->capacity = capacity;
	ht->size = 0;
	ht->total = 0;
	ht->true_coll = 0;

	return ht;
}

int ht_insert(HashTable* ht, uint64_t hash, size_t key, size_t len, StringPool* str_pool) {
	if (ht->size >= ht->capacity) { fprintf(stderr, "hash table full\n"); return 1; }

	// NOTE we can speed this up by using a ht size that is a power of 2 and using a bit mask

	for (size_t i = hash % ht->capacity, j = 0; j < ht->capacity; i = (i + 1) % ht->capacity, j++) { 
		HashEntry* entry = ht->entries + i;

		// if found, increment
		if (entry->count) {
			if (entry->hash == hash)
				{
					const char* entry_str = sp_get(str_pool, entry->key);
					const char* str  = sp_get(str_pool, key);
					if (strcmp(str, entry_str)) {
						// fprintf(stderr, "WARNING: TRUE 64-bit HASH COLLISION with strings: ENTRY |%s| and NEW |%s|\n", entry_str, str);
						ht->true_coll += 1;
					}
					entry->count += 1;
					ht->total += 1;
					return 0;
				}
			else
				continue;
		}

		// empty entry found, insert here
		entry->hash = hash;
		entry->key = key;
		entry->len = len;
		entry->count = 1;
		ht->size += 1;
		ht->total += 1;
		return 0;
	}

	fprintf(stderr, "WARNING Reach end of hash_table in ht_insert without inserting\n");
	return 1;
}

int ht_locked_insert(HashTable* ht, uint64_t hash, size_t key, size_t len, StringPool* str_pool, omp_lock_t* locks, size_t n_locks) {
	if (ht->size >= ht->capacity) { fprintf(stderr, "hash table full\n"); return 1; }
	
	size_t start_i = hash % ht->capacity;
	size_t prev_stripe_id = start_i % n_locks;

	omp_set_lock(locks + prev_stripe_id);
	for (size_t i = start_i, j = 0; j < ht->capacity; i = (i + 1) % ht->capacity, j++) { 
		HashEntry* entry = ht->entries + i;
		size_t stripe_id = i % n_locks;
		
		// lock the next region before any operations
		if (prev_stripe_id != stripe_id) {
			omp_unset_lock(locks + prev_stripe_id);
			omp_set_lock(locks + stripe_id);
			prev_stripe_id = stripe_id;
		}

		// if found, increment
		if (entry->count) {
			if (entry->hash == hash)
				{
					const char* entry_str = sp_get(str_pool, entry->key);
					const char* str  = sp_get(str_pool, key);
					if (strcmp(str, entry_str)) {
						// fprintf(stderr, "WARNING: TRUE 64-bit HASH COLLISION with strings: ENTRY |%s| and NEW |%s|\n", entry_str, str);
						ht->true_coll += 1;
					}
					entry->count += 1;
					ht->total += 1;
					goto ht_locked_insert_success;
				}
			else
				continue;
		}

		// empty entry found, insert here
		entry->hash = hash;
		entry->key = key;
		entry->len = len;
		entry->count = 1;
		ht->size += 1;
		ht->total += 1;
		goto ht_locked_insert_success;
	}

	omp_unset_lock(locks + prev_stripe_id);
	fprintf(stderr, "WARNING Reach end of hash_table in ht_insert without inserting\n");
	return 1;

ht_locked_insert_success:
	omp_unset_lock(locks + prev_stripe_id);
	return 0;
}

void ht_clear(HashTable* ht) {
	for (size_t i = 0; i < ht->capacity; i++) {
		HashEntry* entry = ht->entries + i;
		entry->count = 0;			// The count is what indicates the validity of an entry
	}

	ht->size = 0;
	ht->total = 0;
	ht->true_coll = 0;
}


void ht_print(HashTable* ht) {
	fprintf(stderr, "\t# of Entries: %lu/%lu (%lf%%)\n\tTotal Word Count: %lu\n\tTrue Collisions: %lu\n", ht->size, ht->capacity, (double) ht->size * 100 / ht->capacity, ht->total, ht->true_coll);
}
