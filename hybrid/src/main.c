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


#define GLOBAL_STRIPE_SIZE 		 (1 << 1)
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

enum command_t {
	WORK, DIE
};

typedef struct GlobalMapData {
	char* file_name;
	enum command_t command;
	size_t done;
	size_t working;
	size_t t_ht_capacity;
	size_t t_sp_capacity;
	size_t n_mapper;
	pthread_mutex_t mutex;
	pthread_cond_t cond;
} GlobalMapData;

typedef struct LocalMapData {
	size_t start;
	size_t end;
} LocalMapData;

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

static void map_thread(GlobalMapData* gmd, LocalMapData* lmd) {
	HashTable local_ht;
	if (ht_init(&local_ht, gmd->t_ht_capacity, gmd->t_sp_capacity, 0)) {
		fprintf(stderr, "Thread: %u failed to allocate hashtable\n", omp_get_thread_num());
		return; 
	}
	
	pthread_mutex_lock(&gmd->mutex);
	while(1) {
		pthread_cond_wait(&gmd->cond, &gmd->mutex);
		#pragma omp atomic
		gmd->working += 1;

		pthread_mutex_unlock(&gmd->mutex);
		if (gmd->command == DIE) break;
	
		size_t start = lmd->start;
		size_t end = lmd->end; // this will be calculated by the coordinator thread now
		//size_t end = (j == n_readlocal_ht_threads - 1) ? filesize : file_offsets[j + 1];
		//
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
				if (len > 0)
					{ ht_insert(&local_ht, fnv_hash(buf, len), buf, len); len = 0; }
			}
			else if (len == READBUFSIZE)
				{ ht_insert(&local_ht, fnv_hash(buf, len), buf, len); len = 0; }
			else
				buf[len++] = c;
		}

		read_end = omp_get_wtime();
		fprintf(stderr, "Thread %u took %lf seconds to readlocal_ht to local table\n", omp_get_thread_num(), read_end - read_start);

		fclose(f);		

		// do work
		#pragma omp atomic
		gmd->done += 1;

		fprintf(stderr, "thread %u printing hashtable:\n", omp_get_thread_num());
		ht_print(&local_ht);
	}
}

static void coordinator_thread(int n_files, char** file_names, GlobalMapData* gmd, LocalMapData* lmd) {
	size_t* file_offsets = malloc(sizeof(size_t) * gmd->n_mapper);

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

		fprintf(stderr, "mapper threads finished\n");
	}

	gmd->command = DIE;
	while (gmd->working != gmd->n_mapper) {	
		pthread_cond_broadcast(&gmd->cond);
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

	const size_t available = omp_get_max_threads() - 1;
	const size_t n_mapper = available / 2;
	const size_t n_reducer = available - n_mapper;
	
	GlobalMapData gmd = {
		.file_name = NULL,
		.command = WORK,
		.t_ht_capacity = HT_CAPACITY / n_mapper,
		.t_sp_capacity = SP_CAPACITY / n_mapper,
		.n_mapper = n_mapper,
		.mutex = PTHREAD_MUTEX_INITIALIZER,
		.cond = PTHREAD_COND_INITIALIZER,
		.done = 0,
	};

	LocalMapData* lmd = malloc(sizeof(LocalMapData) * n_mapper);
	if (!lmd) { fprintf(stderr, "Failed to allcoate MapData array\n"); return 1; }

	HashTable reduce_ht;
	if (ht_init(&reduce_ht, HT_CAPACITY, SP_CAPACITY, 1)) { free(lmd); fprintf(stderr, "Failed to allcoate reduce table\n"); return 1; }

	double start, stop;
	start = omp_get_wtime();

	#pragma omp parallel
	{
		#pragma omp single
		{
			for (size_t i = 0; i < n_mapper; i++) {
				#pragma omp task
				map_thread(&gmd, lmd);
			}

			for (size_t i = 0; i < n_reducer; i++) {
				#pragma omp task
				fprintf(stderr, "spawn a reducer thread here\n");
			}

			coordinator_thread(argc - 1, argv + 1, &gmd, lmd);
		}
	}

	// map_reduce(argc - 1, argv + 1);

	stop = omp_get_wtime();

	fprintf(stderr, "Total Time: %lf\n", stop - start);
}

