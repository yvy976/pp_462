#include <stdio.h>
#include <stdlib.h>
#include <omp.h>
#include <mpi.h>
#include <pthread.h>
#include <string.h>
#include <time.h>

#include "fnv_hash.h"
#include "hashtable.h"
#include "str_pool.h"
#include "double_table.h"

#define GLOBAL_STRIPE_SIZE 		 (1 << 6)
#define GLOBAL_HT_CAPACITY_MUL (1 << 8)		// Defines a multiplier for the global
																					// map size as a multiple of PER THREAD
																					// map sizes (HT_CAPACITY / N_THREADS)

#define HT_CAPACITY (1 << 24)							// Total Capacity for all threads combined
																					// (ENTRIES NOT BYTES)

#define READBUFSIZE (1 << 6)							// BYTES

#define GLOBAL_SP_CAPACITY_MUL (1 << 6)		// Defines a multiplier for the global
																					// map's string pool as a multiple of PER THREAD
																					// String Pool sizes

#define SP_CAPACITY (1 << 26)							// Total SP Capacity for all threads combined
																					// BYTES

#define RD_HT_CAPACITY (1 << 24) 						// Reducer table capacity spread across all processes

#define RD_SP_CAPACITY (1 << 26)						

#define SENDWORD_CAPACITY (1 << 6)
#define SENDBUF_CAPACITY (1 << 10)

#define FLUSH_THRESHOLD 0.50

int rank, world_size;

enum command_t {
	WORK, DIE
};

typedef struct GlobalMapData GlobalMapData;
typedef struct LocalMapData LocalMapData;
typedef struct SendEntry SendEntry;
typedef struct SendBuf SendBuf;

struct GlobalMapData {
	char* file_name;
	enum command_t command;
	size_t done;
	size_t working;
	size_t t_ht_capacity;
	size_t t_sp_capacity;
	size_t n_mapper;
	pthread_mutex_t mutex;
	pthread_cond_t cond;
	HashTable* g_ht;
	SendBuf* sb;
};

struct LocalMapData {
	size_t start;
	size_t end;
};

struct SendEntry {
	size_t count;
	uint64_t hash;
	size_t len;
	char str[SENDWORD_CAPACITY];
};

struct SendBuf {
	int working;
	HashTable* table;
	omp_lock_t table_lock;

	enum command_t command;
	pthread_mutex_t mutex;
	pthread_cond_t cond;
	
	size_t n_dests;
	size_t* buf_sizes;
	size_t buf_capacities;
	SendEntry** process_buffers;

};

static inline char to_lower(char c) {
	return ('a' - 'A') | c;
}

int is_alphanum(char c) {
	int num = (c >= '0' && c <= '9');

	char lower = to_lower(c);
	int alpha = (lower >= 'a' && lower <= 'z');

	return alpha || num;
}

static size_t align_word(FILE* f, size_t initial_start, size_t filesize) {
    fseek(f, initial_start, SEEK_SET);
    int c;

    // Skip until next whitespace
		while (1) {	
				if ((c = fgetc(f)) != EOF && is_alphanum(c)) {
					initial_start += 1;
					if (initial_start >= filesize) return filesize;
				} else { fseek(f, -1, SEEK_CUR); break; }
		}
		
    // Skip whitespace until first word character
	  // this one doesn't need to worry about the cursor position after loop end  
		while ((c = fgetc(f)) != EOF && !is_alphanum(c)) {
        initial_start += 1;
        if (initial_start >= filesize) return filesize;
    }

    return initial_start;	
}

// calculates starts on word boundaries
static void calculate_start_offsets(FILE* f, size_t filesize, size_t* file_offsets, size_t n_chunks) {
	// offset for chunk 0
	file_offsets[0] = 0;
	int c;
	while ((c = fgetc(f)) != EOF && !is_alphanum(c)) {		// keep skipping non-alphanum until
		file_offsets[0] += 1;
	};

	// offsets for other chunks
	for (size_t i = 1; i < n_chunks; i++) {
		file_offsets[i] = align_word(f, i * (filesize / n_chunks), filesize);
	}
}

static void flush_global(GlobalMapData* gmd) {
	fprintf(stderr, "atetmpting to flush global frame: %p at %lu / %lu\n", (void*) gmd->g_ht, gmd->g_ht->size, gmd->g_ht->capacity);

	SendBuf* sb = gmd->sb;
	omp_set_lock(&sb->table_lock);
	
	// notify the send thread
	// maybe do this more than once?
	sb->working = 0;
	while (sb->working < 1) {	
		pthread_cond_broadcast(&sb->cond);
		nanosleep(&(struct timespec) { 0, 100000 }, NULL);
	}

	omp_unset_lock(&sb->table_lock);
}

static void flush_to_global(HashTable* local_ht, GlobalMapData* gmd) {
		fprintf(stderr, "flushing local to global: %lu /  %lu\n", local_ht->size, local_ht->capacity);
		// flush to main table
		double map_flush_start, map_flush_end;
		
		map_flush_start = omp_get_wtime();
		
		HashTable* g_ht = gmd->g_ht;

		for (size_t i = 0, count = 0; i < local_ht->capacity && count < local_ht->size; i++) {
			HashEntry* entry = local_ht->entries + i;
			if (!entry->count) continue;

			if (g_ht->size > (size_t) (g_ht->capacity * FLUSH_THRESHOLD)) {
				flush_global(gmd);
			}

			ht_locked_insert(gmd->g_ht, entry->hash, sp_get(local_ht->sp, entry->sp_offset), entry->len, entry->count);

			count += 1;
		}

		map_flush_end = omp_get_wtime();

		fprintf(stderr, "It took %lf to flush local to global\n", map_flush_end - map_flush_start);
}

static void map_thread(GlobalMapData* gmd, LocalMapData* lmd) {
	HashTable local_ht;
	if (ht_init(&local_ht, gmd->t_ht_capacity, gmd->t_sp_capacity, 0)) {
		fprintf(stderr, "Thread: %u failed to allocate hashtable\n", omp_get_thread_num());
		return; 
	}
	
	pthread_mutex_lock(&gmd->mutex);
	while(1) {
		pthread_cond_wait(&gmd->cond, &gmd->mutex);
		int rank;
		MPI_Comm_rank(MPI_COMM_WORLD, &rank);
		fprintf(stderr, "rank %d map thread %u got signaled: %d\n", rank, omp_get_thread_num(), gmd->command);
		pthread_mutex_unlock(&gmd->mutex);

		#pragma omp atomic
		gmd->working += 1;

		if (gmd->command == DIE) {
			fprintf(stderr, "map flushing before death\n");

			flush_to_global(&local_ht, gmd);

			#pragma omp atomic
			gmd->done += 1;
			break;
		}
		fprintf(stderr, "processing file: %s\n", gmd->file_name);
	
		size_t start = lmd->start;
		size_t end = lmd->end; // this will be calculated by the coordinator thread now
		FILE* f = fopen(gmd->file_name, "rb");
		if (!f) { fprintf(stderr, "Failed to open file %s on iteration %u\n", gmd->file_name, omp_get_thread_num()); continue; }
		fseek(f, start, SEEK_SET);

		size_t len = 0;
		char buf[READBUFSIZE];
		int c = fgetc(f);
		
		//
		// PARSING/READING/MAPPING LOOP
		//
		double read_start, read_end;
		read_start = omp_get_wtime();

		for (size_t offset = start; offset < end && c != EOF; offset++, c = fgetc(f)) {
			if (!is_alphanum(c)) {
				if (len > 0) {
						ht_insert(&local_ht, fnv_hash(buf, len), buf, len, 1);
						len = 0;
						if ((double) local_ht.size > (double) local_ht.capacity * FLUSH_THRESHOLD)
							flush_to_global(&local_ht, gmd);
					}
			}
			else if (len == READBUFSIZE)
				{
					ht_insert(&local_ht, fnv_hash(buf, len), buf, len, 1);
					len = 0;
					if ((double) local_ht.size > (double) local_ht.capacity * FLUSH_THRESHOLD)
						flush_to_global(&local_ht, gmd);
				}
			else
				buf[len++] = c;
		}

		read_end = omp_get_wtime();
		fprintf(stderr, "Thread %u took %lf seconds to readlocal_ht to local table\n", omp_get_thread_num(), read_end - read_start);

		fclose(f);
		
		if ((double) local_ht.size > (double) local_ht.capacity * FLUSH_THRESHOLD)
			flush_to_global(&local_ht, gmd);
		
		// do work
		#pragma omp atomic
		gmd->done += 1;

		fprintf(stderr, "thread %u printing hashtable:\n", omp_get_thread_num());
		ht_print(&local_ht);

		// clear local table
		// ht_clear(&local_ht);
	}
}

static void send_thread(GlobalMapData* gmd) {
	SendBuf* sb = gmd->sb;

	pthread_mutex_lock(&sb->mutex);
	while (1) {
		pthread_cond_wait(&sb->cond, &sb->mutex);
		pthread_mutex_unlock(&sb->mutex);

		fprintf(stderr, "send thread got signaled, command: %d\n", sb->command);
		
		#pragma omp atomic
		sb->working += 1;

		if (sb->command == DIE) break;

		// swap
		
		HashTable* tmp = gmd->g_ht;
		
		#pragma omp atomic write
			gmd->g_ht = sb->table;
		
		sb->table = tmp;

		// flush
		omp_set_lock(&sb->table_lock);
		HashTable* table = sb->table;
		
		while (table->n_reference > 0) {	
			nanosleep(&(struct timespec) { 0, 100000 }, NULL);
		}

		while (1) {};

		for (size_t i = 0, count = 0; i < table->capacity && count < table->size; i++) {
			HashEntry* entry = table->entries + i;
			if (!entry->count) continue;
			
			size_t proc = entry->hash % sb->n_dests;
			if (proc == (size_t) rank) {
				fprintf(stderr, "send to local reduce\n");
				continue;
			}
			SendEntry* buf = sb->process_buffers[proc];

			if (sb->buf_sizes[proc] == sb->buf_capacities) {
				fprintf(stderr, "attempting send to: %lu\n", proc);

				MPI_Send(buf, sizeof(SendBuf) * sb->buf_capacities, MPI_CHAR, proc, 3, MPI_COMM_WORLD); 
				sb->buf_sizes[proc] = 0;
			}
			buf += sb->buf_sizes[proc];

			// insertion
			buf->count = entry->count;
			buf->hash = entry->hash;
			buf->len = entry->len;
			strncpy(buf->str, sp_get(sb->table->sp, entry->sp_offset), SENDBUF_CAPACITY);

			sb->buf_sizes[proc] += 1;

			count += 1;
		}

		ht_clear(table);
		omp_unset_lock(&sb->table_lock);

		fprintf(stderr, "send buf resetting\n");
	}

	fprintf(stderr, "send buf exiting\n");
}

static void coordinator_thread(int n_files, char** file_names, GlobalMapData* gmd, LocalMapData* lmd) {
	size_t* file_offsets = malloc(sizeof(size_t) * gmd->n_mapper);
	SendBuf* sb = gmd->sb;

	// for now:
	for (int i = 0; i < n_files; i++) {
		char* file_name = file_names[i];
		fprintf(stderr, "Attempting file: %s\n", file_name);

		FILE* f = fopen(file_name, "rb");
		if (!f) { fprintf(stderr, "Failed to open file %s\n", file_name); return; }
		
		// Get file size
		if (fseek(f, 0, SEEK_END) == -1) { fprintf(stderr, "Failed to seek to end\n"); return; }
		const size_t filesize = ftell(f);
		if (filesize == (size_t) -1) { fprintf(stderr, "Failed to fseek\n"); return; }
		if (fseek(f, 0, SEEK_SET) == -1) { fprintf(stderr, "Failed to seek back to beginning\n"); return; }

		// Calculate offsets on word boundaries
		calculate_start_offsets(f, filesize, file_offsets, gmd->n_mapper);
		fclose(f);
		
		lmd[0].start = 0;
		for (size_t i = 1; i < gmd->n_mapper; i++) {
			lmd[i - 1].end = file_offsets[i];
			lmd[i].start = file_offsets[i];
		}
		lmd[gmd->n_mapper - 1].end = filesize;
		
		// set final global data
		gmd->file_name = file_name;

		// broadcast
		while(gmd->working != gmd->n_mapper) {
			pthread_cond_broadcast(&gmd->cond);
			nanosleep(&(struct timespec) { 0, 100000 }, NULL);
		}
		
		// check incoming file requests here but just rest for now
		while (gmd->done != gmd->n_mapper) {
			nanosleep(&(struct timespec) { 0, 100000 }, NULL);
		}

		// reset counter states
		gmd->working = 0;
		gmd->done = 0;

		fprintf(stderr, "Global State:\n");
		ht_print(gmd->g_ht);

		fprintf(stderr, "mapper threads finished\n");
	}

	gmd->done = 0;
	gmd->command = DIE;
	while (gmd->done < gmd->n_mapper) {
		pthread_cond_broadcast(&gmd->cond);
		nanosleep(&(struct timespec) { 0, 100000 }, NULL);
	}
	
	sb->command = DIE;
	sb->working = 0;
	while (sb->working < 1) {	
		pthread_cond_broadcast(&sb->cond);
		nanosleep(&(struct timespec) { 0, 100000 }, NULL);
	}

	fprintf(stderr, "exiting coordinator\n");

	free(file_offsets);
} 

int main(int argc, char** argv) {
	if (argc < 2) {
		fprintf(stderr, "Usage: wc [FILE1] [FILE2] [FILE{n}...]\n");
		return 0;
	}

	if (omp_get_max_threads() < 3) {
		fprintf(stderr, "atleast 3 threads required\n");
		return 1;
	}

	// MPI init
	int provided = 100;
	MPI_Init_thread(NULL, NULL, MPI_THREAD_MULTIPLE, &provided);
	if (provided < MPI_THREAD_MULTIPLE) {
		fprintf(stderr, "Couldn't get required MPI_THREAD level\n");
		return 1;
	}

	MPI_Comm_rank(MPI_COMM_WORLD, &rank);
	MPI_Comm_size(MPI_COMM_WORLD, &world_size);

	const size_t available = omp_get_max_threads() - 2;		// coordination thread and send thread are taken
	const size_t n_mapper = available / 2;
	const size_t n_reducer = available - n_mapper;

	LocalMapData* lmd = malloc(sizeof(LocalMapData) * n_mapper);
	if (!lmd) { fprintf(stderr, "Failed to allcoate MapData array\n"); return 1; }

	// global mapping table
	HashTable g_ht;
	if (ht_init(&g_ht, HT_CAPACITY, SP_CAPACITY, 1)) { free(lmd); fprintf(stderr, "Failed to allcoate reduce table\n"); return 1; }
	

	// Initialize Send buffer
	SendBuf sb;
	sb.working = 0;
	sb.table = ht_new(HT_CAPACITY, SP_CAPACITY, 1);
	if (!sb.table) { fprintf(stderr, "Failed to allocate g_ht swap table\n"); }
	
	pthread_mutex_init(&sb.mutex, NULL);
	pthread_cond_init(&sb.cond, NULL);

	sb.n_dests = world_size;		// build in the current rank to keep the indexing
	sb.buf_capacities = SENDBUF_CAPACITY;

	omp_init_lock(&sb.table_lock);
	
	sb.buf_sizes = calloc(sb.n_dests, sizeof(size_t));
	if (!sb.buf_sizes) { fprintf(stderr, "Failed to allocate sb.buf_sizes"); return 1;
	}

	sb.process_buffers = malloc(sizeof(void*) * sb.n_dests);
	if (!sb.process_buffers) { fprintf(stderr, "Failed to allocate sb.process_buffers\n"); return 1;}
	
	for (size_t i = 0; i < sb.n_dests; i++) {
		if (i == (size_t) rank) {
			sb.process_buffers[i] = NULL;
			continue;
		}

		sb.process_buffers[i] = malloc(sizeof(SendEntry) * sb.buf_capacities);
		if (!sb.process_buffers[i]) { fprintf(stderr, "Failed to allocate process buffer %lu for the process buffers\n", i); return 1; }
	}

	// global mapping data
	GlobalMapData gmd = {
		.file_name = NULL,
		.command = WORK,
		.t_ht_capacity = HT_CAPACITY / n_mapper,
		.t_sp_capacity = SP_CAPACITY / n_mapper,
		.n_mapper = n_mapper,
		.mutex = PTHREAD_MUTEX_INITIALIZER,
		.cond = PTHREAD_COND_INITIALIZER,
		.done = 0,
		.g_ht = &g_ht,
		.sb = &sb, 
	};

	
	// Spawn threads

	double start, stop;
	start = omp_get_wtime();

	#pragma omp parallel
	{
		#pragma omp single
		{
			#pragma omp task
			send_thread(&gmd);

			for (size_t i = 0; i < n_mapper; i++) {
				#pragma omp task
				map_thread(&gmd, lmd);
			}

			for (size_t i = 0; i < n_reducer; i++) {
				#pragma omp task
				fprintf(stderr, "spawn a reducer thread here\n");
			}

			coordinator_thread(argc - 1, argv + 1, &gmd, lmd);

			fprintf(stderr, "Global Table Final: \n");
			ht_print(gmd.g_ht); 
		}
	}

	// map_reduce(argc - 1, argv + 1);

	stop = omp_get_wtime();

	fprintf(stderr, "Total Time: %lf\n", stop - start);

	MPI_Finalize();
}

