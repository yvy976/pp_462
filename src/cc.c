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
} HashMap;

HashMap *createHashMap(int n) {
    HashMap *map = malloc(sizeof(HashMap));
    map->buckets = calloc(n, sizeof(int));
    map->iarr = calloc(n, sizeof(char *));
    map->unique_word_count = 0;
    map->total_word_count = 0;
    return map;
}

void freeHashMap(HashMap *map, int n) {
    if (!map) return;
    for (int i = 0; i < n; ++i) {
        if (map->iarr[i]) free(map->iarr[i]);
    }
    free(map->iarr);
    free(map->buckets);
    free(map);
}

/* Queue */
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
    for (int i = 0; i < task_count; ++i) {
        int idx = q->pos++;
        q->q[idx] = tasks[i];
    }
    q->count += task_count;
}

int Dequeue(Queue *q, Task **out_tasks) {
    int i;
    for (i = 0; i < BATCH_SIZE && q->count > 0; ++i) {
        int idx = q->top++;
        out_tasks[i] = q->q[idx];
        q->q[idx] = NULL;
        q->count--;
    }
    return i;
}

/* Murmur-ish hash */
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
    if (fseek(fp, offset, SEEK_SET) != 0) { perror("fseek"); return q; }

    if (offset != 0) {
        int c;
        while ((c = fgetc(fp)) != EOF) {
            if (ispunct(c) || isspace(c) || c == '\n' || c == '\0') break;
        }
    }

    char buf[WORD_BUF];
    int idx = 0;
    int c;
    Task *batch[BATCH_SIZE];
    int batch_count = 0;

    while ((c = fgetc(fp)) != EOF) {
        if (ispunct(c) || isspace(c) || c == '\n' || c == '\0') {
            if (idx > 0) {
                buf[idx] = '\0';
                batch[batch_count++] = createTask(buf, idx, 1);
                if (batch_count == BATCH_SIZE) { Enqueue(q, batch, BATCH_SIZE); batch_count = 0; }
            }
            idx = 0;
            if (ftell(fp) > (long)end) break;
        } else {
            if (idx < WORD_BUF - 1) buf[idx++] = tolower((unsigned char)c);
            else {
                while ((c = fgetc(fp)) != EOF && !(ispunct(c) || isspace(c) || c == '\n' || c == '\0'));
                buf[idx] = '\0';
                batch[batch_count++] = createTask(buf, idx, 1);
                if (batch_count == BATCH_SIZE) { Enqueue(q, batch, BATCH_SIZE); batch_count = 0; }
                idx = 0;
                if (ftell(fp) > (long)end) break;
            }
        }
    }
    if (batch_count > 0) Enqueue(q, batch, batch_count);
    return q;
}

/* Merge src map into dst map in parallel */
void mergeHashMapsParallel(HashMap *dst, HashMap *src, int n_dst, int n_src) {
    #pragma omp parallel for schedule(static)
    for (int i = 0; i < n_src; ++i) {
        if (!src->iarr[i]) continue;
        const char *word = src->iarr[i];
        int count = src->buckets[i];
        int idx = (int)hash(word, strlen(word), n_dst);
        while (1) {
            int prev_idx = idx;
            #pragma omp critical
            {
                if (dst->iarr[idx] == NULL) {
                    dst->iarr[idx] = strdup(word);
                    dst->buckets[idx] = count;
                    dst->unique_word_count++;
                    dst->total_word_count += count;
                    idx = -1;
                } else if (strcmp(dst->iarr[idx], word) == 0) {
                    dst->buckets[idx] += count;
                    dst->total_word_count += count;
                    idx = -1;
                } else {
                    idx = (idx + 1) % n_dst;
                }
            }
            if (idx == -1) break;
            if (idx == prev_idx) break; // fail-safe to avoid infinite loop
        }
    }
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "Usage: %s filename\n", argv[0]); return 1; }

    FILE *fp = fopen(argv[1], "rb");
    if (!fp) { perror("fopen"); return 1; }
    if (fseek(fp, 0, SEEK_END) != 0) { perror("fseek"); fclose(fp); return 1; }
    unsigned long long fsize = ftell(fp);
    fclose(fp);

    int nt = omp_get_max_threads();
    unsigned long long read_size = (unsigned long long)ceil((double)fsize / nt);
    if (read_size <= 0) read_size = 1;

    double start_time = omp_get_wtime();
    HashMap *local_maps[nt];

#pragma omp parallel
    {
        int tid = omp_get_thread_num();
        unsigned long long start_off = read_size * tid;
        unsigned long long end_off = start_off + read_size;
        if (end_off > fsize) end_off = fsize;

        FILE *fp_local = fopen(argv[1], "rb");
        if (!fp_local) { perror("fopen thread"); exit(1); }

        Queue *q = ProcessWord(fp_local, start_off, end_off, 1024);
        fclose(fp_local);

        HashMap *map = createHashMap(TABLE_SIZE / nt);
        Task *batch[BATCH_SIZE];
        int got;
        while ((got = Dequeue(q, batch)) > 0) {
            for (int i = 0; i < got; ++i) {
                Task *t = batch[i];
                int idx = (int)hash(t->word, (size_t)t->len, TABLE_SIZE / nt);
                while (1) {
                    if (map->iarr[idx] == NULL) {
                        map->iarr[idx] = strdup(t->word);
                        map->buckets[idx] = t->count;
                        break;
                    } else if (strcmp(map->iarr[idx], t->word) == 0) {
                        map->buckets[idx] += t->count;
                        break;
                    } else {
                        idx = (idx + 1) % (TABLE_SIZE / nt);
                    }
                }
                free(t->word);
                free(t);
            }
        }
        local_maps[tid] = map;
    }

    // logarithmic parallel aggregation
    int step = 1;
    while (step < nt) {
        for (int i = 0; i + step < nt; i += step << 1) {
            mergeHashMapsParallel(local_maps[i], local_maps[i + step], TABLE_SIZE, TABLE_SIZE / nt);
        }
        step <<= 1;
    }

    HashMap *global_map = local_maps[0];

    FILE *fw = fopen("pppp.txt", "w");
    for (int i = 0; i < TABLE_SIZE; ++i) {
        if (global_map->iarr[i])
            fprintf(fw, "%s: %d\n", global_map->iarr[i], global_map->buckets[i]);
    }
    fclose(fw);

    double end_time = omp_get_wtime();
    printf("File: %s\nThreads: %d\nTotal Time: %f\nTotal: %ld Unique: %ld\n",
           argv[1], nt, end_time - start_time,
           global_map->total_word_count, global_map->unique_word_count);

    for (int i = 1; i < nt; ++i)
        freeHashMap(local_maps[i], TABLE_SIZE / nt);

    freeHashMap(global_map, TABLE_SIZE);
    return 0;
}

