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
	StringPool* sp;
	omp_lock_t* locks;
	size_t n_locks
	size_t stripe_size;

	size_t capacity;
	size_t size;
	size_t total;
	size_t true_coll;
};
*/

void ht_free(HashTable* ht) {
	free(ht->entries);
	sp_free(ht->sp);

	if (ht->locks) {
		for (size_t i = 0; i < ht->n_locks; i++) {
			omp_destroy_lock(ht->locks + i);
		}
		free(ht->locks);
	}

	free(ht);
}

// if stripe size is 0, it isn't concurrent and shouldn't be called with locked functions
HashTable* ht_new(size_t ht_capacity, size_t sp_capacity, size_t stripe_size) {
	HashTable* ht = malloc(sizeof(*ht));
	if (!ht) return NULL;

	ht->entries = calloc(ht_capacity, sizeof(*ht->entries));
	if (!ht->entries) { free(ht); return NULL; }

	ht->sp = sp_new(sp_capacity);
	if (!ht->sp) { free(ht->entries); free(ht); return NULL; }

	if (stripe_size) {
		ht->n_locks = ht_capacity / stripe_size;
		if (ht_capacity % stripe_size) ht->n_locks += 1;

		ht->locks = malloc(sizeof(*ht->locks) * ht->n_locks);
		if (!ht->locks) { sp_free(ht->sp); free(ht->entries); free(ht); return NULL; }
		
		for (size_t i = 0; i < ht->n_locks; i++) {
			omp_init_lock(ht->locks + i);
		}

		ht->stripe_size = stripe_size;
	}
	else { ht->locks = NULL; ht->stripe_size = 0; ht->n_locks = 0; }

	ht->capacity = ht_capacity;
	ht->size = 0;
	ht->total = 0;
	ht->true_coll = 0;

	return ht;
}

int ht_insert(HashTable* ht, uint64_t hash, const char* str, size_t len) {
	if (ht->size >= ht->capacity) { fprintf(stderr, "hash table full\n"); return 1; }
	
	for (size_t i = hash % ht->capacity, j = 0; j < ht->capacity; i = (i + 1) % ht->capacity, j++) { 
		HashEntry* entry = ht->entries + i;

		// if found, increment
		if (entry->count) {
			if (entry->hash == hash)
					{
					const char* str = sp_get(ht->sp, entry->sp_offset);
					if (strncmp(str, str, len)) {
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
		entry->sp_offset = sp_add(ht->sp, str, len);
		entry->len = len;
		entry->count = 1;
		ht->size += 1;
		ht->total += 1;
		return 0;
	}

	fprintf(stderr, "WARNING Reach end of hash_table in ht_insert without inserting\n");
	return 1;
}

int ht_locked_insert(HashTable* ht, uint64_t hash, const char* str, size_t len) {
	if (ht->size >= ht->capacity) { fprintf(stderr, "hash table full\n"); return 1; }
	
	size_t start_i = hash % ht->capacity;
	size_t prev_stripe_id = start_i % ht->n_locks;

	omp_set_lock(ht->locks + prev_stripe_id);
	for (size_t i = start_i, j = 0; j < ht->capacity; i = (i + 1) % ht->capacity, j++) { 
		HashEntry* entry = ht->entries + i;
		size_t stripe_id = i % ht->n_locks;
		
		// lock the next region before any operations
		if (prev_stripe_id != stripe_id) {
			omp_unset_lock(ht->locks + prev_stripe_id);
			omp_set_lock(ht->locks + stripe_id);
			prev_stripe_id = stripe_id;
		}

		// if found, increment
		if (entry->count) {
			if (entry->hash == hash)
				{
					const char* entry_str = sp_get(ht->sp, entry->sp_offset);
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
		entry->sp_offset = sp_add(ht->sp, str, len);
		entry->len = len;
		entry->count = 1;
		ht->size += 1;
		ht->total += 1;
		goto ht_locked_insert_success;
	}

	omp_unset_lock(ht->locks + prev_stripe_id);
	fprintf(stderr, "WARNING Reach end of hash_table in ht_insert without inserting\n");
	return 1;

ht_locked_insert_success:
	omp_unset_lock(ht->locks + prev_stripe_id);
	return 0;
}

void ht_clear(HashTable* ht) {
	sp_clear(ht->sp);

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
