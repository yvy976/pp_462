#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <omp.h>
#include <math.h>
#include <ctype.h>
#define TABLE_SIZE 7500000
#define SEED 1
#define BATCH_SIZE 1024
#define WORD_BUF 64
/* --------------------------Task-------------------------- */
typedef struct
{
    char *word;
    int len;
    int count;
} Task;

Task *createTask(char *word, int len, int count)
{
    Task *t = malloc(sizeof(Task));
    t->word = strdup(word);
    t->len = len;
    t->count = count;

    return t;
}
/* --------------------------Task-------------------------- */

/* --------------------------HashMap-------------------------- */
typedef struct
{
    int total_word_count;
    int unique_word_count;
    int *buckets;
    char **iarr;
} HashMap;

HashMap *createHashMap(int n)
{
    HashMap *map = malloc(sizeof(HashMap));
    map->buckets = calloc(n, sizeof(int));
    map->iarr = calloc(n, sizeof(char *));
    map->unique_word_count = 0;
    map->total_word_count = 0;
    return map;
}
/* --------------------------HashMap-------------------------- */

/* --------------------------Queue-------------------------- */
typedef struct
{
    int count;
    int pos;
    int top;
    int len;
    Task **q;
} Queue;

Queue *createWorkQueue(int n)
{
    Queue *q = malloc(sizeof(Queue));
    q->pos = 0;
    q->count = 0;
    q->top = 0;
    q->len = 0;
    q->q = calloc(n, sizeof(Task *));
    return q;
}

/* -------Enqueue------- */
void Enqueue(Queue *q, Task **tasks, int task_count, int len)
{

    for (int i = 0; i < task_count; i++)
    {
        int idx = q->pos ;
        q->q[idx] = tasks[i];
        q->pos++;
    }
    q->count += task_count;
}
/* -------Enqueue------- */

/* -------Dequeue------- */
int Dequeue(Queue *q, Task **out_tasks, int len)
{

    int i;
    for (i = 0; i < BATCH_SIZE && q->count > 0; i++)
    {
        int idx = q->top;
        out_tasks[i] = q->q[idx];
        q->q[idx] = NULL;
        q->top++;
        q->count--;
    }

    /* returns number of tasks dequeued */
    return i;
}
/* -------Dequeue------- */

/* --------------------------Queue-------------------------- */

/* --------------------------Hash function-------------------------- */
unsigned long long _Hash_bytes(const void *ptr, size_t len, size_t seed, size_t modValue)
{
    const size_t m = 0x5bd1e995;
    unsigned long long hash = seed ^ len;
    const char *buf = (const char *)ptr;
    while (len >= 4)
    {
        // size_t k = unaligned_load(buf);
        unsigned int k = buf[0] | (buf[1] << 8) | (buf[2] << 16) | (buf[3] << 24);
        k *= m;
        k ^= k >> 24;
        k *= m;
        hash *= m;
        hash ^= k;
        buf += 4;
        len -= 4;
    }

    // Handle the last few bytes of the input array.
    switch (len)
    {
    case 3:
        hash ^= (unsigned char)(buf[2]) << 16;
        /* FALLTHROUGH */
    case 2:
        hash ^= (unsigned char)(buf[1]) << 8;
        /* FALLTHROUGH */
    case 1:
        hash ^= (unsigned char)(buf[0]);
        hash *= m;
    };

    // Do a few final mixes of the hash.
    hash ^= hash >> 13;
    hash *= m;
    hash ^= hash >> 15;
    return hash % modValue;
}

/* Hashing helper function */
unsigned int hash(const void *word, size_t len, size_t modValue)
{
    return (unsigned int)_Hash_bytes(word, len, SEED, modValue);
}
/* --------------------------Hash function-------------------------- */

/* --------------------------Process words function-------------------------- */
Queue *ProcessWord(FILE *fp, unsigned long long offset, unsigned long long end, size_t size_per_thread)
{
    Queue *q = createWorkQueue(size_per_thread);
    fseek(fp, offset, SEEK_SET);
    if (offset != 0)
    {
        int c;
        while ((c = fgetc(fp)) != EOF)
        {
            if (ispunct(c) || c == '\0' || c == '\n' || isspace(c))
                break;
        }
    }

    char buf[WORD_BUF];
    int idx = 0;
    char c;
    Task *batch[BATCH_SIZE];
    int batch_count = 0;

    while ((c = fgetc(fp)) != EOF)
    {
        if (ispunct(c) || c == '\0' || c == '\n' || isspace(c))
        {
            buf[idx] = '\0';

            batch[batch_count++] = createTask(buf, idx, 1);

            if (batch_count == BATCH_SIZE)
            {
                Enqueue(q, batch, BATCH_SIZE, size_per_thread);
                batch_count = 0;
            }

            idx = 0;
            bzero(buf, sizeof(buf));
            if (ftell(fp) > end)
                break;
        }
        else
        {
            buf[idx++] = tolower(c);
        }
    }

    if (batch_count > 0)
        Enqueue(q, batch, batch_count, size_per_thread);
    return q;
}
/* --------------------------Process words function-------------------------- */

/* Minimum */
int min(int a, int b)
{
    return a < b ? a : b;
}
/* Minimum */

/* --------------------------Main function-------------------------- */
int main(int argc, char **argv)
{
    double start_time, end_time;
    unsigned long long end = 0;
    HashMap *global_map = createHashMap(TABLE_SIZE);
    FILE *fp = fopen(argv[1], "r");
    fseek(fp, 0, SEEK_END);
    end = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    fclose(fp);
    volatile int done = 0;
    start_time = omp_get_wtime();

        HashMap *map = createHashMap(TABLE_SIZE);

        FILE *fp_local = fopen(argv[1], "r");
  double reader_start = omp_get_wtime();
        Queue *q = ProcessWord(fp_local, 0, end, TABLE_SIZE);
double reader_end = omp_get_wtime();
        fclose(fp_local);

        done += 1;
        Task *batch[BATCH_SIZE];
        int got;
    double mapper_start = omp_get_wtime();
        while (1)
        {
            got = Dequeue(q, batch, TABLE_SIZE);
            if (got == 0)
            {
                if (done == 1)
                    break;
                continue;
            }
            for (int i = 0; i < got; ++i) {
                Task *t = batch[i];
                unsigned int idx = hash(t->word, (size_t)t->len, (size_t)TABLE_SIZE);
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
                        idx = (idx + 1) % TABLE_SIZE;
                    }
                }
                free(t->word);
                free(t);
            }
        }

double mapper_end = omp_get_wtime();
        
double agg_start = omp_get_wtime();
            for (int i = 0; i < TABLE_SIZE; i++)
         {
            if (map->iarr[i] == NULL) continue;
            const char *word = map->iarr[i];
            int word_count = map->buckets[i];
            int cur = (int)hash(word, strlen(word), TABLE_SIZE);
            while (1) {
                if (global_map->iarr[cur] == NULL) {
                    global_map->iarr[cur] = strdup(word);
                    global_map->buckets[cur] = word_count;
//                    global_map->unique_word_count += 1;
//                    global_map->total_word_count += word_count;
                    break;
                } else if (strcmp(global_map->iarr[cur], word) == 0) {
                    global_map->buckets[cur] += word_count;
//                    global_map->total_word_count += word_count;
                    break;
                } else {
                    cur = (cur + 1) % TABLE_SIZE;
                }
            }
        }


          double agg_end = omp_get_wtime();

    end_time = omp_get_wtime();
printf("Reader. Time: %f\n", reader_end - reader_start);
printf("Mapper. Time: %f\n", mapper_end - mapper_start);
printf("Aggr. Time: %f\n", agg_end - agg_start);

    FILE *f = fopen("pppp.txt", "w");
    for (int i = 0; i < TABLE_SIZE; i++)
    {
        if (global_map->iarr[i] != 0)
        {
            fprintf(f, "%s: %d\n", global_map->iarr[i], global_map->buckets[i]);
        }
    }
    fclose(f);
    double total_time = end_time - start_time;
    printf("File: %s\nThreads: 1\nTotal Time: %f\nTotal: %d Unique: %d\nBatch Size: %d\n\n", argv[1], total_time, global_map->total_word_count, global_map->unique_word_count, BATCH_SIZE);
    fflush(stdout);
    return 0;
}
/* --------------------------Main function-------------------------- */
