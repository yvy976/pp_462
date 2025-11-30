#include <string.h>
#include <stdlib.h>
#include "send_queue.h"

#define SENDQUEUE_CAPACITY (1 << 6)
#define MAXSTR_CAPACITY (1 << 5)

/*
struct SendEntry {
	char str[MAXSTR_CAPACITY];
	uint64_t hash;
	size_t len;
	size_t count;
};

struct SendQueue {
	MPI_Request request;
	SendEntry* buffer;
	size_t capacity;
	size_t size;
};
*/

SendQueue* sq_new(size_t capacity, int send_dst) {
	SendQueue* sq = malloc(sizeof(*sq));
	if (!sq) return NULL;

	sq->buffer = malloc(sizeof(*sq->buffer) * capacity);
	if (!sq->buffer) { free(sq); return NULL; }

	sq->request = NULL;
	sq->size = 0;
	sq->capacity = capacity;
	sq->send_dst = send_dst;

	return sq;
}

void sq_free(SendQueue* sq) {
	free(sq->buffer);
	free(sq);
}

void sq_poll(SendQueue* sq) {
	int completed;
	MPI_Test(&sq->request, &completed, MPI_STATUS_IGNORE);
	
	if (!completed) return;

	sq->size = 0;
	sq->request = NULL;
}

int sq_push(SendQueue* sq, HashEntry* entry, StringPool* sp) {
		if (sq->request) {
			sq_poll(sq);
			return 1;
		}
		else if (sq->size == sq->capacity) {
			MPI_Isend(sq->buffer, sizeof(*sq->buffer), MPI_CHAR, sq->send_dst, 1, MPI_COMM_WORLD, &sq->request);
			return 1;
		}

	SendEntry* send_entry = sq->buffer + sq->size;
	
	strncpy(send_entry->str, sp_get(sp, entry->sp_offset), MAXSTR_CAPACITY - 1);
	send_entry->str[MAXSTR_CAPACITY - 1] = '\0';
	send_entry->hash = entry->hash;
	send_entry->count = entry->count;
	send_entry->len = entry->len;

	return 0;
}
