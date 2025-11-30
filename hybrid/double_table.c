#include <stdlib.h>
#include <stdio.h>
#include "double_table.h"

void dt_free(DoubleTable* dt) {
	ht_free(dt->forward);
	ht_free(dt->back);
	omp_destroy_lock(&dt->lock);
	free(dt);
}

DoubleTable* dt_new(size_t ht_capacity, size_t sp_capacity, double threshold) {
	DoubleTable* dt = malloc(sizeof(DoubleTable));
	if (!dt) return NULL;

	dt->forward = ht_new(ht_capacity, sp_capacity, 1);
	if (!dt->forward) { free(dt); return NULL; }

	dt->back = ht_new(ht_capacity, sp_capacity, 1);
	if (!dt->back) { free(dt->forward); free(dt); return NULL; }
	
	omp_init_lock(&dt->lock);
	dt->flush_cursor = 0;
	dt->flushing = 1;
	dt->capacity = ht_capacity;
	dt->threshold = threshold * ht_capacity;
	return dt;
}

static void _dt_swap(DoubleTable* dt) {
	fprintf(stderr, "table swapped\n");
	HashTable* tmp = dt->forward;
	dt->forward = dt->back;
	dt->back = tmp;
}

int dt_insert(DoubleTable* dt, uint64_t hash, const char* str, size_t len) {
	// swap buffers if over threshold and not in the middle of a flush
	if (dt->forward->size >= dt->threshold && !dt->flushing) {
		_dt_swap(dt);
	}
	
	ht_locked_insert(dt->forward, hash, str, len);
	return 0;
}

void dt_print(DoubleTable* dt) {
	fprintf(stderr, "\tthreshold: %ld\n\tflushing: %d\t\n:flush_cursor: %lu\nforward table:\n", dt->threshold, dt->flushing, dt->flush_cursor);
	ht_print(dt->forward);
	fprintf(stderr, "dt->backward:\n");
	ht_print(dt->back);
}
