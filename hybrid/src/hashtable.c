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

void ht_deinit(HashTable* ht) {
	free(ht->entries);
	sp_free(ht->sp);

	if (ht->locks) {
		for (size_t i = 0; i < ht->n_locks; i++) {
			omp_destroy_lock(ht->locks + i);
		}
		free(ht->locks);
	}
}

void ht_free(HashTable* ht) {
	ht_deinit(ht);
	free(ht); 
}

int ht_init(HashTable* ht, size_t ht_capacity, size_t sp_capacity, size_t stripe_size) {
	if (!ht) return 1;

	ht->entries = calloc(ht_capacity, sizeof(*ht->entries));
	if (!ht->entries) { return 1; }

	ht->sp = sp_new(sp_capacity, stripe_size);
	if (!ht->sp) { free(ht->entries); return 1; }

	if (stripe_size) {
		ht->n_locks = ht_capacity / stripe_size;
		if (ht_capacity % stripe_size) ht->n_locks += 1;

		ht->locks = malloc(sizeof(*ht->locks) * ht->n_locks);
		if (!ht->locks) { sp_free(ht->sp); free(ht->entries); return 1; }
		
		for (size_t i = 0; i < ht->n_locks; i++) {
			omp_init_lock(ht->locks + i);
		}

		ht->stripe_size = stripe_size;
		ht->n_reference = 0;
	}
	else { ht->locks = NULL; ht->stripe_size = 0; ht->n_locks = 0; }

	ht->capacity = ht_capacity;
	ht->size = 0;
	ht->total = 0;
	ht->true_coll = 0;

	return 0;
}

// if stripe size is 0, it isn't concurrent and shouldn't be called with locked functions
HashTable* ht_new(size_t ht_capacity, size_t sp_capacity, size_t stripe_size) {
	HashTable* ht = malloc(sizeof(HashTable));
	if (!ht || ht_init(ht, ht_capacity, sp_capacity, stripe_size)) { free(ht); return NULL; }

	return ht;
}

int ht_insert(HashTable* ht, uint64_t hash, const char* str, size_t len, size_t count) {
	if (ht->size >= ht->capacity) { fprintf(stderr, "hash table full\n"); return 1; }
	
	for (size_t i = hash % ht->capacity, j = 0; j < ht->capacity; i = (i + 1) % ht->capacity, j++) { 
		HashEntry* e = ht->entries + i;

		if (!e->count) {
			e->count = count;
			e->sp_offset = sp_add(ht->sp, str, len);
			e->hash = hash;
			e->len = len;

			ht->size += 1;
			ht->total += count;

			return 0;
		}
		else if (e->hash != hash) { continue; }
		else if (strcmp(str, sp_get(ht->sp, e->sp_offset))) {
			ht->true_coll += 1;
			continue;
		}
		else {
			e->count += count;
			ht->total += count;

			return 0;
		}
	}

	fprintf(stderr, "WARNING Reach end of hash_table in ht_insert without inserting\n");
	return 1;
}

int ht_locked_insert(HashTable* ht, uint64_t hash, const char* str, size_t len, size_t count) {
	#pragma omp atomic
	ht->n_reference += 1;

	size_t cap = ht->capacity;
	
	for (size_t i = hash % cap, j = 0; j < cap; i = (i + 1) % cap, j++) {
		HashEntry* e = ht->entries + i;

		size_t stripe_id = i / ht->stripe_size;
	
		omp_set_lock(&ht->locks[stripe_id]);
		if (!e->count) {
			e->count = count;
			e->sp_offset = sp_add(ht->sp, str, len);
			e->hash = hash;
			e->len = len;
			
			omp_unset_lock(&ht->locks[stripe_id]);
			
			#pragma omp atomic
			ht->size += 1;

			#pragma omp atomic
			ht->total += count;
			
			#pragma omp atomic
			ht->n_reference -= 1;

			return 0;
		}
		else if (e->hash != hash) { omp_unset_lock(&ht->locks[stripe_id]); continue; }
		else if (strcmp(str, sp_get(ht->sp, e->sp_offset))) {
			omp_unset_lock(&ht->locks[stripe_id]);
			#pragma omp atomic
				ht->true_coll += 1;
			continue;
		}
		else {
			e->count += count;
			omp_unset_lock(&ht->locks[stripe_id]);
			
			#pragma omp atomic
			ht->total += count; 

			#pragma omp atomic
			ht->n_reference -= 1;
			return 0;
		}
	}

	fprintf(stderr, "couldn't find place in table, table full\n");

	#pragma omp atomic
	ht->n_reference += 1;

	return 1;
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
