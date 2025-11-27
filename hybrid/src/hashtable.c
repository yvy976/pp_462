#include <stdlib.h>
#include <string.h>
#include <stdio.h> 			// DEBUGGING
#include "hashtable.h"

/*
struct HashEntry {
	uint64_t hash;
	const char* key;
	size_t count;
};

struct HashTable {
	HashEntry* entries;
	size_t capacity;
	size_t size;
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

	return ht;
}

int ht_insert(HashTable* ht, uint64_t hash, size_t val, StringPool* str_pool) {
	if (ht->size >= ht->capacity) { fprintf(stderr, "hash table full\n"); return 1; }

	// NOTE we can speed this up by using a ht size that is a power of 2 and using a bit mask

	for (size_t i = hash % ht->capacity, j = 0; j < ht->capacity; i = (i + 1) % ht->capacity, j++) { 
		HashEntry* entry = ht->entries + i;

		// if found, increment
		if (entry->key) {
			if (entry->hash == hash)
				{
					const char* entry_str = sp_get(str_pool, entry->key);
					const char* str  = sp_get(str_pool, val);
					if (strcmp(str, entry_str)) {
						fprintf(stderr, "WARNING: TRUE 64-bit HASH COLLISION with strings: ENTRY |%s| and NEW |%s|\n", entry_str, str);
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
		entry->key = val;
		entry->count = 1;
		ht->size += 1;
		ht->total += 1;
		return 0;
	}

	fprintf(stderr, "WARNING Reach end of hash_table in ht_insert without inserting\n");
	return 1;
}
