#include <stdio.h>
#include <stdlib.h>
#include <omp.h>
#include <mpi.h>

#include "fnv_hash.h"
#include "hashtable.h"
#include "str_pool.h"

#define HT_CAPACITY (1 << 23)				// Total Capacity for all threads combined
																		// (ENTRIES NOT BYTES)

#define READBUFSIZE (1 << 6)				// BYTES

#define SP_CAPACITY (1 << 20)				// Total SP Capacity for all threads combined
																		// BYTES

enum ReadState {
	CONSUME,			// consume whitespace
	PROCESS				// process word
};

inline char to_lower(char c) {
	return ('a' - 'A') | c;
}

int is_alphanum(char c) {
	int num = (c >= '0' && c <= '9');

	char lower = to_lower(c);
	int alpha = (lower >= 'a' && lower <= 'z');

	return alpha || num;
}

size_t align_word(FILE* f, size_t initial_start, size_t filesize) {
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
void calculate_start_offsets(FILE* f, size_t filesize, size_t* file_offsets, size_t n_chunks) {
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

int map_reduce(size_t file_count, char** file_names) {
	size_t n_readmap_threads = (size_t) omp_get_max_threads();
	size_t* file_offsets = malloc(sizeof(*file_offsets) * n_readmap_threads);
	StringPool** str_pools = malloc(sizeof(*str_pools) * n_readmap_threads);
	HashTable** maps = malloc(sizeof(HashTable*) * n_readmap_threads);

	// Allocate Maps
	for (size_t i = 0; i < n_readmap_threads; i++) {
		if (!(maps[i] = ht_new(HT_CAPACITY / n_readmap_threads)))
				{ fprintf(stderr, "Failed to allocate hashtable for %lu\n", i); return 1; } 
		if (!(str_pools[i] = sp_new(SP_CAPACITY / n_readmap_threads))) { fprintf (stderr, "Failed to allocate str pool for %lu\n", i); return 1; }
	}
	
	for (size_t i = 0; i < file_count; i++) { 
		//
		// First file pass for information and chunk boundaries
		//
		const char* file_name = file_names[i];
		fprintf(stderr, "Attempting file: %s\n", file_name);

		FILE* f = fopen(file_name, "r");
		if (!f) { fprintf(stderr, "Failed to open file %s\n", file_name); return 1; }
		
		// Get file size
		if (fseek(f, 0, SEEK_END) == -1) { fprintf(stderr, "Failed to seek to end\n"); return 1; }
		const size_t filesize = ftell(f);
		if (filesize == (size_t) -1) { fprintf(stderr, "Failed to fseek\n"); return 1; }
		if (fseek(f, 0, SEEK_SET) == -1) { fprintf(stderr, "Failed to seek back to beginning\n"); return 1; }

		// Calculate offsets on word boundaries
		calculate_start_offsets(f, filesize, file_offsets, n_readmap_threads);

		fclose(f);

		// DEBUG PRINT
		for (size_t i = 0; i < n_readmap_threads; i++) {
			fprintf(stderr, "File offset for %lu: %lu (from %lu)\n", i, file_offsets[i], i * (filesize / n_readmap_threads));
		}

		#pragma omp parallel for
		for (size_t i = 0; i < n_readmap_threads; i++) {
			size_t start = file_offsets[i];
			size_t end = (i == n_readmap_threads - 1) ? filesize : file_offsets[i + 1];

			FILE* f = fopen(file_name, "r");
			if (!f) { fprintf(stderr, "Failed to open file %s on iteration %lu\n", file_name, i); continue; }
			fseek(f, start, SEEK_SET);

			size_t len = 0;
			char buf[READBUFSIZE];
			enum ReadState state = PROCESS;
			int c = fgetc(f);
			for (size_t offset = start; offset < end && c != EOF; offset++, c = fgetc(f)) {
				switch (state) {
					case PROCESS:
						// fprintf(stderr, "i: %lu, PROCESS: %c\n", i, c);
						if (!is_alphanum(c)) {
							size_t sp_offset = sp_add(str_pools[i], buf, len);
							// fprintf(stdout, "i: %lu, inserted string: %s\n", i, sp_get(sp, sp_offset);
							ht_insert(maps[i], fnv_hash(buf, len), sp_offset, str_pools[i]);
							len = 0;
							fseek(f, -1, SEEK_CUR);
							offset -= 1;
							state = CONSUME;
						} else {
							buf[len++] = c;
						}
						break;
					case CONSUME:
						// fprintf(stderr, "i: %lu, PROCESS %c\n", i, c);
						if (is_alphanum(c)) {
							fseek(f, -1, SEEK_CUR);
							offset -= 1;
							state = PROCESS;
						}
						break;
				}
			}
		}
	}
	//
	// CLEAN
	//
	
	for (size_t i = 0; i < n_readmap_threads; i++) {
		ht_free(maps[i]);
		sp_free(str_pools[i]);
	}
	free(maps);
	free(str_pools);
	free(file_offsets); 
	return 0;
}

int main(int argc, char** argv) {
	if (argc < 2) {
		fprintf(stderr, "Usage: wc [FILE1] [FILE2] [FILE{n}...]\n");
		return 0;
	}
	
	double start, stop;
	start = MPI_Wtime();

	map_reduce(argc - 1, argv + 1);

	stop = MPI_Wtime();

	fprintf(stderr, "Total Time: %lf\n", stop - start);
}

