#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <omp.h>
#include <math.h>

#define TABLE_SIZE 7500000
#define SEED 1
#define BATCH_SIZE 1024
#define WORD_BUF 16

typedef struct {
    char *word;
    int len;
    int count;
} Task;

Task *createTask(const char *word, int len, int count) {
    Task *t = malloc(sizeof(Task));
    t->word = strndup(word, len);
    t->len = len;
    t->count = count;
    return t;
}

/* Hash map */
typedef struct {
    long total_word_count;
    long unique_word_count;
    int *buckets;
    char **iarr;
    omp_lock_t *locks;   // lock array (striped)
} HashMap;

HashMap *createHashMap(int n, int nlocks) {
    HashMap *map = malloc(sizeof(HashMap));
    map->buckets = calloc(n, sizeof(int));
    map->iarr = calloc(n, sizeof(char *));
    map->unique_word_count = 0;
    map->total_word_count = 0;
    map->locks = malloc(nlocks * sizeof(omp_lock_t));
    for (int i = 0; i < nlocks; ++i) omp_init_lock(&map->locks[i]);
    return map;
}

/* Queue (single-producer single-consumer per-thread usage) */
typedef struct {
    int count;
    int pos;
    int top;
    int len;
    Task **q;
} Queue;

Queue *createWorkQueue(int n) {
    Queue *q = malloc(sizeof(Queue));
    q->pos = q->top = q->count = 0;
    q->len = n;
    q->q = calloc(n, sizeof(Task *));
    return q;
}


void Enqueue(Queue *q, Task **tasks, int task_count) {
    if (task_count <= 0) return;
#pragma omp parallel for 
    for (int i = 0; i < task_count; ++i) {
        int idx = q->pos ;
        if (q->q[idx]) {
            free(q->q[idx]->word);
            free(q->q[idx]);
        }
        q->q[idx] = tasks[i];
        q->pos++;
    }
    q->count += task_count;
}

int Dequeue(Queue *q, Task **out_tasks) {
    int i;
    for (i = 0; i < BATCH_SIZE && q->count > 0; ++i) {
        int idx = q->top ;
        out_tasks[i] = q->q[idx];
        q->q[idx] = NULL;
        q->top++;
        q->count--;
    }
    return i;
}

unsigned long long _Hash_bytes(const void *ptr, size_t len, size_t seed, size_t modValue) {
    const size_t m = 0x5bd1e995;
    unsigned long long hash = seed ^ len;
    const unsigned char *buf = (const unsigned char *)ptr;
    while (len >= 4) {
        unsigned int k = buf[0] | (buf[1] << 8) | (buf[2] << 16) | (buf[3] << 24);
        k *= m;
        k ^= k >> 24;
        k *= m;
        hash *= m;
        hash ^= k;
        buf += 4;
        len -= 4;
    }
    switch (len) {
        case 3: hash ^= (unsigned long long)buf[2] << 16; [[fallthrough]];
        case 2: hash ^= (unsigned long long)buf[1] << 8; [[fallthrough]];
        case 1: hash ^= (unsigned long long)buf[0]; hash *= m;
    }
    hash ^= hash >> 13;
    hash *= m;
    hash ^= hash >> 15;
    return (modValue == 0) ? hash : (hash % modValue);
}
unsigned int hash(const void *word, size_t len, size_t modValue) {
    return (unsigned int)_Hash_bytes(word, len, SEED, modValue);
}

/* Process a file segment into a per-thread queue of tasks */
Queue *ProcessWord(FILE *fp, unsigned long long offset, unsigned long long end, int queue_len) {
    Queue *q = createWorkQueue(queue_len);
    if (fseek(fp, offset, SEEK_SET) != 0) {
        perror("fseek");
        return q;
    }

    if (offset != 0) { // advance to next separator so we start at token boundary
        int c;
        while ((c = fgetc(fp)) != EOF) {
            if (ispunct(c) || c == '\0' || c == '\n' || isspace(c)) break;
        }
    }

    char buf[WORD_BUF];
    int idx = 0;
    int c;
    Task *batch[BATCH_SIZE];
    int batch_count = 0;

    while ((c = fgetc(fp)) != EOF) {
        if (ispunct(c) || c == '\0' || c == '\n' || isspace(c)) {
            if (idx > 0) {                      // skip empty tokens
                buf[idx] = '\0';
                batch[batch_count++] = createTask(buf, idx, 1);
                if (batch_count == BATCH_SIZE) {
                    Enqueue(q, batch, BATCH_SIZE);
                    batch_count = 0;
                }
            }
            idx = 0;
            if (ftell(fp) > (long)end) break;
        } else {
            if (idx < WORD_BUF - 1) buf[idx++] = tolower((unsigned char)c);
            else {
                // word too long -> truncate
                // continue consuming until separator
                while ((c = fgetc(fp)) != EOF && !(ispunct(c) || c == '\0' || c == '\n' || isspace(c)));
                if (c == EOF || ftell(fp) > (long)end) break;
                // finalize truncated word
                buf[idx] = '\0';
                batch[batch_count++] = createTask(buf, idx, 1);
                if (batch_count == BATCH_SIZE) { Enqueue(q, batch, BATCH_SIZE); batch_count = 0; }
                idx = 0;
            }
        }
    }
    if (batch_count > 0) Enqueue(q, batch, batch_count);
    return q;
}

/* main */
int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "Usage: %s filename\n", argv[0]); return 1; }
    double end_time;

    int nt = omp_get_max_threads();
    unsigned long long fsize = 0;

    FILE *fp = fopen(argv[1], "rb");
    if (!fp) { perror("fopen"); return 1; }
    if (fseek(fp, 0, SEEK_END) != 0) { perror("fseek"); fclose(fp); return 1; }
    fsize = ftell(fp);
    fclose(fp);

    // Global table with striped locks (nlocks = nt)


    volatile int done = 0;
    int size_per_thread = TABLE_SIZE / nt;
    if (size_per_thread <= 0) size_per_thread = 1;
    unsigned long long read_size = (unsigned long long)ceil((double)fsize / (double)nt);
    if (read_size <= 0) read_size = 1;
   double start_time = omp_get_wtime();
   HashMap *global_map = createHashMap(TABLE_SIZE, TABLE_SIZE);
#pragma omp parallel shared(done)
    {
        int tid = omp_get_thread_num();
        // per-thread private map. Use int cast but ensure not overflow.
        HashMap *map = createHashMap(read_size, 0);

        unsigned long long thread_read_section = read_size * tid;
        unsigned long long thread_read_section_end = read_size == 0 ? fsize : (unsigned long long) (thread_read_section + read_size < fsize ? thread_read_section + read_size : fsize);

        FILE *fp_local = fopen(argv[1], "rb");
        if (!fp_local) { perror("fopen thread"); exit(1); }

#pragma omp barrier
        double reader_start = omp_get_wtime();
        Queue *q = ProcessWord(fp_local, thread_read_section, thread_read_section_end, size_per_thread);
        double reader_end = omp_get_wtime();

        fclose(fp_local);

#pragma omp atomic
        done++;
#pragma omp flush(done)

        Task *batch[BATCH_SIZE];
        int got;

#pragma omp barrier
        double mapper_start = omp_get_wtime();

        while (1) {
            got = Dequeue(q, batch);
            if (got == 0) {
#pragma omp flush(done)
                if (done == nt) break;
                continue;
            }
            for (int i = 0; i < got; ++i) {
                Task *t = batch[i];
                unsigned int idx = hash(t->word, (size_t)t->len, (size_t)read_size);
                // linear probing within local map (local_n slots)
                while (1) {
                    if (map->iarr[idx] == NULL) {
                        map->iarr[idx] = strdup(t->word);
                        map->buckets[idx] = t->count;
                        break;
                    } else if (strcmp(map->iarr[idx], t->word) == 0) {
                        map->buckets[idx] += t->count;
                        break;
                    } else {
                        idx = (idx + 1) % read_size;
                    }
                }
                free(t->word);
                free(t);
            }
        }
#pragma omp barrier
        double mapper_end = omp_get_wtime();

        // Aggregate local map into global_map
#pragma omp barrier
        double agg_start = omp_get_wtime();
        for (int i = 0; i < read_size; ++i)
         {
            if (map->iarr[i] == NULL) continue;
            const char *word = map->iarr[i];
            int word_count = map->buckets[i];
            int cur = (int)hash(word, strlen(word), TABLE_SIZE);
            // linear probe on global, locking per-slot (striped)
            while (1) {
                int lock_idx = (cur * nt) / TABLE_SIZE;
                omp_set_lock(&global_map->locks[lock_idx]);
                if (global_map->iarr[cur] == NULL) {
                    global_map->iarr[cur] = strdup(word);
                    global_map->buckets[cur] = word_count;
                    omp_unset_lock(&global_map->locks[lock_idx]);
                 //  #pragma omp atomic
                    global_map->unique_word_count += 1;
                  // #pragma omp atomic
                    global_map->total_word_count += word_count;
                    break;
                } else if (strcmp(global_map->iarr[cur], word) == 0) {
                    global_map->buckets[cur] += word_count;
                    omp_unset_lock(&global_map->locks[lock_idx]);
                  // #pragma omp atomic
                    global_map->total_word_count += word_count;
                    break;
                } else {
                    omp_unset_lock(&global_map->locks[lock_idx]);
                    cur = (cur + 1) % TABLE_SIZE;
                }
            }
        }
        double agg_end = omp_get_wtime();

    end_time = omp_get_wtime();
#pragma omp single
        {
            printf("Reader. Time: %f\n", reader_end - reader_start);
            printf("Mapper. Time: %f\n", mapper_end - mapper_start);
            printf("Aggr. Time: %f\n", agg_end - agg_start);
        }

        // cleanup per-thread
    } // omp parallel

    // write global results
    FILE *fw = fopen("pppp.txt", "w");
    for (int i = 0; i < TABLE_SIZE; ++i) {
        if (global_map->iarr[i] != NULL) {
            fprintf(fw, "%s: %d\n", global_map->iarr[i], global_map->buckets[i]);
        }
    }
    fclose(fw);

    printf("File: %s\nThreads: %d\nTotal Time: %f\nTotal: %ld Unique: %ld\nBatch Size: %d\n\n",
           argv[1], omp_get_max_threads(), end_time - start_time,
           global_map->total_word_count, global_map->unique_word_count, BATCH_SIZE);

    return 0;
}

