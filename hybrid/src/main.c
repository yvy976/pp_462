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

#define GLOBAL_STRIPE_SIZE 		 (1 << 6)
#define GLOBAL_HT_CAPACITY_MUL (1 << 8)		// Defines a multiplier for the global
																					// map size as a multiple of PER THREAD
																					// map sizes (HT_CAPACITY / N_THREADS)

#define HT_CAPACITY (1 << 25)							// Total Capacity for all threads combined
																					// (ENTRIES NOT BYTES)

#define READBUFSIZE (1 << 6)							// BYTES

#define GLOBAL_SP_CAPACITY_MUL (1 << 6)		// Defines a multiplier for the global
																					// map's string pool as a multiple of PER THREAD
																					// String Pool sizes

#define SP_CAPACITY (1 << 26)							// Total SP Capacity for all threads combined
																					// BYTES

#define RD_HT_CAPACITY (1 << 28) 						// Reducer table capacity spread across all processes

#define RD_SP_CAPACITY (1l << 32)						

#define SENDWORD_CAPACITY (1 << 6)
#define SENDBUF_CAPACITY (1 << 20)

#define OUTFORMAT_SIZE (1lu << 32)

#define FLUSH_THRESHOLD 0.7

int rank, world_size;

enum command_t {
	WORK, DIE
};

typedef struct GlobalMapData GlobalMapData;
typedef struct LocalMapData LocalMapData;
typedef struct SendEntry SendEntry;
typedef struct SendBuf SendBuf;
typedef struct RecvBuf RecvBuf;
typedef struct RecvData RecvData;
typedef struct Stats Stats;

struct Stats {
	double total_time;
	double RM_time;				// Total time spent reading/mapping
	double L_to_G_time;		// Total time spent flushing local maps to global maps
	double G_to_N_time;		// Total time spent flushing the global map to the network
	double R_to_R_time;		// Total time spend pushing recived entries into the reducer table
};

struct GlobalMapData {
	char* file_name;
	enum command_t command;
	size_t done;
	size_t working;
	size_t t_ht_capacity;
	size_t t_sp_capacity;
	size_t n_mapper;
	size_t n_reducer;
	pthread_mutex_t mutex;
	pthread_cond_t cond;
	HashTable* g_ht;
	SendBuf* sb;
	RecvData* recv_data;
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
	int signal;
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

struct RecvBuf {
	size_t size;
	size_t capacity;
	SendEntry* buffer;
};

// There needs to be a way to communicate with the thread
struct RecvData {
	enum command_t command;
};

Stats	stats;

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

static void try_flush_global(GlobalMapData* gmd) {
// 	fprintf(stderr, "atetmpting NONBLOCKING global frame: %p at %lu / %lu\n", (void*) gmd->g_ht, gmd->g_ht->size, gmd->g_ht->capacity);

	SendBuf* sb = gmd->sb;
	if (!omp_test_lock(&sb->table_lock)) {
		return;
	} else if (sb->working) {
		omp_unset_lock(&sb->table_lock);
		return;
	}
	// notify the send thread
	// maybe do this more than once?
	sb->working = 1;
	sb->signal = 0; 
	while (sb->signal < 1) {	
		pthread_cond_broadcast(&sb->cond);
		nanosleep(&(struct timespec) { 0, 100000 }, NULL);
	}

	omp_unset_lock(&sb->table_lock);
}

static void flush_global(GlobalMapData* gmd) {
	// fprintf(stderr, "atetmpting BLOCKING FLUSH global frame: %p at %lu / %lu\n", (void*) gmd->g_ht, gmd->g_ht->size, gmd->g_ht->capacity);
	// fprintf(stderr, "table before flush\n");
	// ht_print(gmd->g_ht);

	SendBuf* sb = gmd->sb;
	omp_set_lock(&sb->table_lock);
	
	// notify the send thread
	// maybe do this more than once?
	sb->signal = 0;
	while (sb->signal < 1) {	
		pthread_cond_broadcast(&sb->cond);
		nanosleep(&(struct timespec) { 0, 100000 }, NULL);
	}

	omp_unset_lock(&sb->table_lock);
}

static void flush_to_global(HashTable* local_ht, GlobalMapData* gmd) {
	//fprintf(stderr, "flushing local to global: %lu /  %lu\n", local_ht->size, local_ht->capacity);
		// ht_print(local_ht);
		// flush to main table
		double map_flush_start, map_flush_end;
		
		map_flush_start = omp_get_wtime();
		
		HashTable* g_ht = gmd->g_ht;

		for (size_t i = 0, count = 0; i < local_ht->capacity && count < local_ht->size; i++) {
			HashEntry* entry = local_ht->entries + i;
			if (!entry->count) continue;

			if (g_ht->size > (size_t) (g_ht->capacity * FLUSH_THRESHOLD)) {
				try_flush_global(gmd);
			}

			ht_locked_insert(gmd->g_ht, entry->hash, sp_get(local_ht->sp, entry->sp_offset), entry->len, entry->count);

			count += 1;
		}

		// fprintf(stderr, "inserted %ld counts\n", count);

		ht_clear(local_ht);

		map_flush_end = omp_get_wtime();
		stats.L_to_G_time += map_flush_end - map_flush_start;

		// fprintf(stderr, "It took %lf to flush local to global\n", map_flush_end - map_flush_start);
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
		// fprintf(stderr, "rank %d map thread %u got signaled: %d\n", rank, omp_get_thread_num(), gmd->command);
		pthread_mutex_unlock(&gmd->mutex);

		#pragma omp atomic
		gmd->working += 1;

		if (gmd->command == DIE) {
			// fprintf(stderr, "rank: %d map flushing before death\n", rank);

			flush_to_global(&local_ht, gmd);

			#pragma omp atomic
			gmd->done += 1;

			// fprintf(stderr, "mapper %u exited\n", omp_get_thread_num());
			break;
		}
		// fprintf(stderr, "processing file: %s\n", gmd->file_name);
		
		size_t count = 0;
		size_t start = lmd->start;
		size_t end = lmd->end; // this will be calculated by the coordinator thread now
		FILE* f = fopen(gmd->file_name, "rb");
		if (!f) { fprintf(stderr, "Failed to open file %s on iteration %u\n", gmd->file_name, omp_get_thread_num()); continue; }
		fseek(f, start, SEEK_SET);

		// fprintf(stderr, "Thread %u working on range: %lu - %lu\n", omp_get_thread_num(), start, end);

		size_t len = 0;
		char buf[READBUFSIZE];
		int c = fgetc(f);
		
		//
		// PARSING/READING/MAPPING LOOP
		//
		// double read_start, read_end;
		// read_start = omp_get_wtime();

		for (size_t offset = start; offset < end && c != EOF; offset++, c = fgetc(f)) {
			if (!is_alphanum(c)) {
				if (len > 0) {
						count += 1;
						ht_insert(&local_ht, fnv_hash(buf, len), buf, len, 1);
						len = 0;
						if ((double) local_ht.size > (double) local_ht.capacity * FLUSH_THRESHOLD ||
								(double) local_ht.sp->used > (double) local_ht.sp->capacity * FLUSH_THRESHOLD)
							flush_to_global(&local_ht, gmd);
					}
			}
			else if (len == READBUFSIZE)
				{
					count += 1;
					ht_insert(&local_ht, fnv_hash(buf, len), buf, len, 1);
					len = 0;
					if ((double) local_ht.size > (double) local_ht.capacity * FLUSH_THRESHOLD ||
							(double) local_ht.sp->used > (double) local_ht.sp->capacity * FLUSH_THRESHOLD)
						flush_to_global(&local_ht, gmd);
				}
			else
				buf[len++] = c;
		}

		// read_end = omp_get_wtime();
		// fprintf(stderr, "Thread %u took %lf seconds to read %lu words to local table\n", omp_get_thread_num(), read_end - read_start, count);

		fclose(f);
		if ((double) local_ht.size > (double) local_ht.capacity * FLUSH_THRESHOLD ||
				(double) local_ht.sp->used > (double) local_ht.sp->capacity * FLUSH_THRESHOLD)
			flush_to_global(&local_ht, gmd);

		// do work
		#pragma omp atomic
		gmd->done += 1;

		//fprintf(stderr, "thread %u printing hashtable:\n", omp_get_thread_num());
		// ht_print(&local_ht);

		// clear local table
		// ht_clear(&local_ht);
	}

	ht_deinit(&local_ht);
}

static void send_thread(GlobalMapData* gmd, HashTable* reducer_table) {
	SendBuf* sb = gmd->sb;

	pthread_mutex_lock(&sb->mutex);
	while (1) {
		#pragma omp atomic write
		sb->working = 0;

		pthread_cond_wait(&sb->cond, &sb->mutex);
		pthread_mutex_unlock(&sb->mutex);

		// fprintf(stderr, "send thread got signaled, command: %d\n", sb->command);
		
		// flush sends before death
		if (sb->command == DIE) {
			for (size_t i = 0; i < sb->n_dests; i++) {
				size_t size = sb->buf_sizes[i];
				// fprintf(stderr, "rank %d sending to proc %ld: %lu entries\n", rank, i, size);
				if (!size || i == (size_t) rank) continue;

				// fprintf(stderr, "MPI SEND CALL: buf: %p, bytes: %lu (%lu entries), to: %lu\n", sb->process_buffers[i], sizeof(SendEntry) * size, size, i);
				MPI_Send(sb->process_buffers[i], sizeof(SendEntry) * size, MPI_BYTE, i, 3, MPI_COMM_WORLD); 
			}

			#pragma omp atomic
				sb->signal += 1;

			#pragma omp atomic write
				sb->working = 0;
			
			// fprintf(stderr, "send_thread exiting\n");
			break;
		} else { 
			#pragma omp atomic
			sb->signal += 1;
		}

		// swap
		HashTable* tmp = gmd->g_ht;

		// #pragma omp atomic write
			gmd->g_ht = sb->table;
		
		sb->table = tmp;

		// flush
		omp_set_lock(&sb->table_lock);
		HashTable* table = sb->table;
		
		while (table->n_reference > 0) {	
			nanosleep(&(struct timespec) { 0, 100000 }, NULL);
		}
		
		// attempting to send
		// fprintf(stderr, "attempting to flush: %lu entries out of table %p\n", table->size, (void*)table);
		// ht_print(table);
		
		double gtn_start, gtn_end;
		gtn_start = omp_get_wtime();

		for (size_t i = 0, count = 0; i < table->capacity && count < table->size; i++) {
			HashEntry* entry = table->entries + i;
			if (!entry->count) continue;
			
			size_t proc = entry->hash % sb->n_dests;
			// fprintf(stderr, "hash: %lx, str: %.*s, dest proc: %lu\n", entry->hash, (int)entry->len, sp_get(table->sp, entry->sp_offset), proc);
			if (proc == (size_t) rank) {
				ht_locked_insert(reducer_table, entry->hash, sp_get(table->sp, entry->sp_offset), entry->len, entry->count);
				continue;
			}
			SendEntry* buf = sb->process_buffers[proc];

			if (sb->buf_sizes[proc] == sb->buf_capacities) {
				// fprintf(stderr, "ATTEMPTING SEND from buffer: %p, bytes: %lu (%lu entries), to proc: %lu\n", buf, sb->buf_capacities * sizeof(SendEntry), sb->buf_capacities, proc);
				MPI_Send(buf, sizeof(SendEntry) * sb->buf_capacities, MPI_BYTE, proc, 3, MPI_COMM_WORLD); 
				sb->buf_sizes[proc] = 0;
			}
			buf += sb->buf_sizes[proc];

			// insertion
			buf->count = entry->count;
			buf->hash = entry->hash;
			buf->len = entry->len;

			if (buf->len >= SENDWORD_CAPACITY) {
				fprintf(stderr, "INTERETING CATCH hash entry with len: %lu\n", buf->len);
			}

			strncpy(buf->str, sp_get(sb->table->sp, entry->sp_offset), entry->len);

			sb->buf_sizes[proc] += 1;

			count += 1;
		}

		gtn_end = omp_get_wtime();
		stats.G_to_N_time += gtn_end - gtn_start;

		ht_clear(table);
		omp_unset_lock(&sb->table_lock);

		// fprintf(stderr, "send buf resetting\n");
	}

	// fprintf(stderr, "send buf exiting\n");
}

static void recieve_thread(HashTable* reducer_table, RecvBuf* rb, RecvData* rd) {
	int flag;
	int size;
	MPI_Status s;
	MPI_Message m;

	while (1) {
		// Probe for message
		MPI_Improbe(MPI_ANY_SOURCE, 3, MPI_COMM_WORLD, &flag, &m, &s);

		if (flag) {
			MPI_Get_count(&s, MPI_BYTE, &size);
			if ((size_t) size > SENDBUF_CAPACITY * sizeof(SendEntry)) {
				fprintf(stderr, "WARNING: incoming message is larger than expected!\n"); 
			} 
		
			// fprintf(stderr, "RECIEVING\n");
			MPI_Mrecv(rb->buffer, size, MPI_BYTE, &m, MPI_STATUS_IGNORE);
			// fprintf(stderr, "recieved %d bytes (%ld entries) from process %d\n", size, size / sizeof(SendEntry), s.MPI_SOURCE);
			
			double insert_start, insert_end;
			insert_start = omp_get_wtime();

			for (size_t i = 0; i < size / sizeof(SendEntry); i++) {
				SendEntry* entry = rb->buffer + i;
				ht_locked_insert(reducer_table, entry->hash, entry->str, entry->len, entry->count);
			}

			insert_end = omp_get_wtime();
			stats.R_to_R_time = insert_end - insert_start;
		} else if (rd->command == DIE) {
			// fprintf(stderr, "recieve %u thread exiting\n", omp_get_thread_num());
			break;
		} else {
			nanosleep(&(struct timespec) { 0, 100000 }, NULL);
		}
	}
}

static void rank0_coordinator_loop(int n_files, char** file_names, GlobalMapData* gmd, LocalMapData* lmd) {
	size_t* file_offsets = malloc(sizeof(size_t) * gmd->n_mapper);
	
	size_t other_active_procs = world_size - 1;

	// for now:
	for (int i = world_size - 1; i < n_files; i++) {
		char* file_name = file_names[i];
		// fprintf(stderr, "Attempting file: %s\n", file_name);

		FILE* f = fopen(file_name, "rb");
		if (!f) { fprintf(stderr, "Failed to open file %s\n", file_name); continue; }
		
		// Get file size
		if (fseek(f, 0, SEEK_END) == -1) { fprintf(stderr, "Failed to seek to end\n"); return; }
		const size_t filesize = ftell(f);
		if (filesize == (size_t) -1) { fprintf(stderr, "Failed to fseek\n"); return; }
		if (fseek(f, 0, SEEK_SET) == -1) { fprintf(stderr, "Failed to seek back to beginning\n"); return; }
		
		// fprintf(stderr, "rank 0 working on file index: %d\n", i);

		// Calculate offsets on word boundaries
		calculate_start_offsets(f, filesize, file_offsets, gmd->n_mapper);

		fclose(f);
		
		lmd[0].start = 0;
		for (size_t j = 1; j < gmd->n_mapper; j++) {
			lmd[j - 1].end = file_offsets[j];
			lmd[j].start = file_offsets[j];
		}
		lmd[gmd->n_mapper - 1].end = filesize;

		// set final global data
		gmd->file_name = file_name;

		// broadcast
		while(gmd->working != gmd->n_mapper) {
			pthread_cond_broadcast(&gmd->cond);
			nanosleep(&(struct timespec) { 0, 100000 }, NULL);
		}
		
		// check incoming file requests
		while (gmd->done != gmd->n_mapper) {
			int flag;
			MPI_Message m;
			MPI_Status s;
			MPI_Improbe(MPI_ANY_SOURCE, 1, MPI_COMM_WORLD, &flag, &m, &s);

			if (flag) {
				// fprintf(stderr, "rank 0 recieved a file request from rank %u\n", s.MPI_SOURCE);
	
				MPI_Mrecv(NULL, 0, MPI_CHAR, &m, MPI_STATUS_IGNORE);

				int data = -1;
				if (i + 1 < n_files) {
					data = ++i;
					
					// fprintf(stderr, "sending back index: %u\n", i);
				} else { other_active_procs -= 1; }
				
				MPI_Send(&data, 1, MPI_INT, s.MPI_SOURCE, 1, MPI_COMM_WORLD);
				
			} else { nanosleep(&(struct timespec) { 0, 100000 }, NULL); }
		}

		// reset counter states
		gmd->working = 0;
		gmd->done = 0;

		// fprintf(stderr, "Global State:\n");
		//ht_print(gmd->g_ht);

		// fprintf(stderr, "mapper threads finished\n");
	}

	free(file_offsets);

	// wait for other processes to finish mapping so I can stop coordinating
	MPI_Status s;
	const int data = -1;
	for (size_t i = 0; i < other_active_procs; i++) { 
		MPI_Recv(NULL, 0, MPI_CHAR, MPI_ANY_SOURCE, 1, MPI_COMM_WORLD, &s);
		MPI_Send(&data, 1, MPI_INT, s.MPI_SOURCE, 1, MPI_COMM_WORLD);
	}
}

static void rankother_coordinator_loop(int n_files, char** file_names, GlobalMapData* gmd, LocalMapData* lmd) { 
	size_t* file_offsets = malloc(sizeof(size_t) * gmd->n_mapper);
	int file_index = rank - 1;

	do {
		//fprintf(stderr, "rank %u is doing file_index %u\n", rank, file_index); 

		char* file_name = file_names[file_index];
		// fprintf(stderr, "Attempting file: %s\n", file_name);

		FILE* f = fopen(file_name, "rb");
		if (!f) {
			fprintf(stderr, "Failed to open file %s\n", file_name);
			goto rankother_coordinator_loop_request;
		}
		
		// Get file size
		if (fseek(f, 0, SEEK_END) == -1) { fprintf(stderr, "Failed to seek to end\n"); return; }
		const size_t filesize = ftell(f);
		if (filesize == (size_t) -1) { fprintf(stderr, "Failed to fseek\n"); return; }
		if (fseek(f, 0, SEEK_SET) == -1) { fprintf(stderr, "Failed to seek back to beginning\n"); return; }
		
		//fprintf(stderr, "Rank %d is working on file index: %d\n", rank, file_index);

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

		// fprintf(stderr, "Global State:\n");
		// ht_print(gmd->g_ht);

		// fprintf(stderr, "mapper threads finished\n");

rankother_coordinator_loop_request:
	
		// REQUEST FOR FILE
		// fprintf(stderr, "rank %d requesting new file index\n", rank);
		MPI_Send(NULL, 0, MPI_CHAR, 0, 1, MPI_COMM_WORLD);

		// fprintf(stderr, "rank %u mpirecv call\n", rank);
		MPI_Recv(&file_index, 1, MPI_INT, 0, 1, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
		
		// fprintf(stderr, "rank %u recieved index %d\n", rank, file_index);

		if (file_index == -1) break;
		if (file_index >= n_files) {
			fprintf(stderr, "recieved a bad file index: %d??\n", file_index);
			break;
		}
	} while (1);
	
	free(file_offsets);
}

static void coordinator_thread(int n_files, char** file_names, GlobalMapData* gmd, LocalMapData* lmd) {
	SendBuf* sb = gmd->sb;
	
	double map_start, map_end;
	map_start = omp_get_wtime();

	if (!rank)
		rank0_coordinator_loop(n_files, file_names, gmd, lmd);
	else
		rankother_coordinator_loop(n_files, file_names, gmd, lmd); 
	
	// signal mappers to flush and die
	gmd->done = 0;
	gmd->command = DIE;
	while (gmd->done < gmd->n_mapper) {
		pthread_cond_broadcast(&gmd->cond);
		nanosleep(&(struct timespec) { 0, 100000 }, NULL);
	}

	map_end = omp_get_wtime();
	stats.RM_time = map_end - map_start;

	// flush the global table
	if (gmd->g_ht->size > 0) {
		flush_global(gmd);
	}

	// Kill the send thread after its done
	sb->command = DIE;
	sb->working = 1;
	sb->signal = 0;
	while (sb->working != 0 || sb->signal < 1) {	
		pthread_cond_broadcast(&sb->cond);
		nanosleep(&(struct timespec) { 0, 100000 }, NULL);
	}
	
	MPI_Barrier(MPI_COMM_WORLD);
			
	// soft request the recieving threads
	for (size_t i = 0; i < gmd->n_reducer; i++) {
		gmd->recv_data[i].command = DIE;
	}

	// fprintf(stderr, "exiting coordinator\n");
} 

int main(int argc, char** argv) {
	if (argc < 3) {
		fprintf(stderr, "Usage: wc [OUTPUT FILE] [FILE1] [FILE2] [FILE{n}...]\n");
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

	// shrink the effective world size if we don't need other processes
	if (argc - 2 < world_size) {
		world_size = argc - 2;
		if (rank >= world_size) {
			fprintf(stderr, "Process %u not necessary, not participating\n", rank);
			MPI_Barrier(MPI_COMM_WORLD); 
			MPI_Finalize();
			return 0;
		}
	}

	const size_t available = omp_get_max_threads() - 2;		// coordination thread and send thread are taken
	const size_t n_mapper = available / 2;
	const size_t n_reducer = available - n_mapper;

	LocalMapData* lmd = malloc(sizeof(LocalMapData) * n_mapper);
	if (!lmd) { fprintf(stderr, "Failed to allcoate MapData array\n"); return 1; }

	// global mapping table
	HashTable* g_ht = ht_new(HT_CAPACITY, SP_CAPACITY, 1);
	if (!g_ht) { fprintf(stderr, "Failed to allcoate global table 1\n"); return 1; }	

	HashTable* g_ht2 = ht_new(HT_CAPACITY, SP_CAPACITY, 1);
	if (!g_ht2) { fprintf(stderr, "Failed to allocate global table 2\n"); return 1; }

	// Initialize Send buffer
	SendBuf sb;
	sb.signal = 0;
	sb.working = 0;
	sb.table = g_ht2;
	// fprintf(stderr, "sb.table: %p\n", (void*) sb.table);

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

	RecvBuf* recv_buffers = malloc(sizeof(RecvBuf) * n_reducer);
	if (!recv_buffers) { fprintf(stderr, "Failed to allocate recv_buffers array\n"); return 1; }

	// for n in number of reducers, allocate buffers
	for (size_t i = 0; i < n_reducer; i++) {
		RecvBuf* buf = recv_buffers + i;
		buf->buffer = malloc(sizeof(SendEntry) * SENDBUF_CAPACITY);
		if (!buf->buffer) { fprintf(stderr, "Failed to allocate the underlying buffer for recvbuf object for reducer: %lu\n", i); return 1;}
		buf->size = 0;
		buf->capacity = SENDBUF_CAPACITY;
	}

	RecvData* recv_data = malloc(sizeof(RecvData) * n_reducer);
	for (size_t i = 0; i < n_reducer; i++) { 
		recv_data[i].command = WORK;
	}

	HashTable reducer_table;
	if (ht_init(&reducer_table, RD_HT_CAPACITY / world_size, RD_SP_CAPACITY / world_size, 1)) {
		fprintf(stderr, "Failed to allocate reducer table\n");
		return 1;
	}

	// global mapping data
	GlobalMapData gmd = {
		.file_name = NULL,
		.command = WORK,
		.t_ht_capacity = HT_CAPACITY / n_mapper,
		.t_sp_capacity = SP_CAPACITY / n_mapper,
		.n_mapper = n_mapper,
		.n_reducer = n_reducer,
		.mutex = PTHREAD_MUTEX_INITIALIZER,
		.cond = PTHREAD_COND_INITIALIZER,
		.done = 0,
		.g_ht = g_ht,
		.sb = &sb, 
		.recv_data = recv_data,
	};

	// Spawn threads

	double start, stop;
	start = omp_get_wtime();

	#pragma omp parallel
	{
		#pragma omp master
		{
			#pragma omp task
			send_thread(&gmd, &reducer_table);

			for (size_t i = 0; i < n_mapper; i++) {
				#pragma omp task
				map_thread(&gmd, lmd + i);
			}
			for (size_t i = 0; i < n_reducer; i++) {
				#pragma omp task
				recieve_thread(&reducer_table, recv_buffers + i, recv_data + i);
			}
			
			coordinator_thread(argc - 2, argv + 2, &gmd, lmd);

			#pragma omp taskwait
			
			// fprintf(stderr, "Rank %u, Global Table Final: \n", rank);
			// ht_print(gmd.g_ht);

			// fprintf(stderr, "Rank %u, SendBuf Table Final: \n", rank);
			// ht_print(sb.table);

			// fprintf(stderr, "Rank %u, Reducer Table Final: \n", rank);
			// ht_print(&reducer_table);
		}

	}

	stop = omp_get_wtime();
	if (!rank) {
		stats.total_time = stop - start;
		fprintf(stderr, "Total Time: %lf\n", stats.total_time);
		fprintf(stderr, "RM Time: %lf\n", stats.RM_time);
		fprintf(stderr, "Local to Global time: %lf\n", stats.L_to_G_time);
		fprintf(stderr, "Global to Netwokr time: %lf\n", stats.G_to_N_time);
		fprintf(stderr, "Recieved Data to Reduce Table time: %lf\n", stats.R_to_R_time);
	}

	// free everything but the reducer table
	free(lmd);
	ht_free(g_ht);
	ht_free(g_ht2);
	
	pthread_mutex_destroy(&sb.mutex);
	pthread_cond_destroy(&sb.cond);
	
	omp_destroy_lock(&sb.table_lock);

	free(sb.buf_sizes);

	for(size_t i = 0; i < sb.n_dests; i++) {
		if (i == (size_t) rank) continue;

		free(sb.process_buffers[i]);
	}
	free(sb.process_buffers);

	for(size_t i = 0; i < n_reducer; i++) {
		RecvBuf* buf = recv_buffers + i;
		free(buf->buffer);
	}
	free(recv_buffers);
	free(recv_data);
	
	// fprintf(stderr, "printing to %s\n", argv[1]);
	// make start formatting output into buffers

	size_t n_threads = omp_get_max_threads();
	size_t buffer_capacity = OUTFORMAT_SIZE / n_threads;
	size_t chunk_size = reducer_table.capacity / n_threads;
	
	FILE* f;
	omp_lock_t* write_lock = & (omp_lock_t) {};
	omp_init_lock(write_lock);

	if (!rank) {
		f = fopen(argv[1], "w");
		if (!f) { fprintf(stderr, "rank 0 failed to open the output file\n"); return 1; }
	} else {
		MPI_Recv(NULL, 0, MPI_CHAR, rank - 1, 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
		f = fopen(argv[1], "a");
		if (!f) { fprintf(stderr, "rank %d failed to open the output file\n", rank); }
	}

	#pragma omp parallel for
	for (size_t i = 0; i < n_threads; i++) {
		
		size_t start = chunk_size * i;
		size_t end = (i + 1 == n_threads) ? reducer_table.capacity : start + chunk_size;
		
		// fprintf(stderr, "threaed: %d doing start %ld end %ld\n", omp_get_thread_num(), start, end);
		size_t used = 0;
		char* buf = malloc(sizeof(char) * buffer_capacity);

		if (!buf) { fprintf(stderr, "Thread %u failed to allocate for print buf\n", omp_get_thread_num()); continue; }
		
		for (size_t j = start; j < end; j++) {
			HashEntry* entry = reducer_table.entries + j;
			if (!entry->count) continue;
			
			int remaining = buffer_capacity - used;
			int n = snprintf(buf + used, remaining, "%.*s: %lu\n", (int) entry->len, sp_get(reducer_table.sp, entry->sp_offset), entry->count);

			if (n < 0) { fprintf(stderr, "snprintf error on thread %u\n", omp_get_thread_num()); continue; }
 			else if (n >= remaining) { used = 0; }
			else used += n;
			// truncated, we should flush the buffer

			if (n >= remaining) {
				omp_set_lock(write_lock);
				if (fwrite(buf, 1, used, f) != used) {
					fprintf(stderr, "failed to write\n");
				}
				omp_unset_lock(write_lock);

				used = 0;
				j--;
			} else {
				used += n;
			}	
		}

		// flush buffers
		if (used) {
			omp_set_lock(write_lock);
			if (fwrite(buf, 1, used, f) != used) {
				fprintf(stderr, "failed to write\n");
			}
			omp_unset_lock(write_lock);

			used = 0;
		}
		free(buf);	
	}

	fclose(f);

	if (rank + 1 < world_size) {
		// fprintf(stderr, "rank %d done, signalling other thread: %d\n", rank, rank + 1);
		MPI_Send(NULL, 0, MPI_CHAR, rank + 1, 0, MPI_COMM_WORLD);
	}

	MPI_Finalize();

	//omp_destroy_lock(&write_lock);
	//ht_deinit(&reducer_table);
}

