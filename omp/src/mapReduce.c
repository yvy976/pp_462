#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <omp.h>
#include <math.h>
#include <ctype.h>
#define TABLE_SIZE 7500000
#define SEED 1
#define BATCH_SIZE 64

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
  omp_lock_t *locks;
} HashMap;

HashMap *createHashMap(int n, int l)
{
  HashMap *map = malloc(sizeof(HashMap));
  map->buckets = calloc(n, sizeof(int));
  map->iarr = calloc(n, sizeof(char *));
  map->unique_word_count = 0;
  map->total_word_count = 0;
  map->locks = malloc(l * sizeof(omp_lock_t));
  for (int i = 0; i < l; i ++) omp_init_lock(&map->locks[i]);
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
    int idx = q->pos % len;
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
    int idx = q->top % len;
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
      [[gnu::fallthrough]];
    case 2:
      hash ^= (unsigned char)(buf[1]) << 8;
      [[gnu::fallthrough]];
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

  char buf[64];
  int idx = 0;
  int c;
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
  int nt = omp_get_max_threads();
  unsigned long long end = 0;

  HashMap *global_map = createHashMap(TABLE_SIZE, nt);
  FILE *fp = fopen(argv[1], "r");
  fseek(fp, 0, SEEK_END);
  end = ftell(fp);
  fseek(fp, 0, SEEK_SET);
  fclose(fp);
  volatile int done = 0;
  int size_per_thread = TABLE_SIZE / nt;
  unsigned long long read_size = ceil((float)end / (nt));
  start_time = omp_get_wtime();
#pragma omp parallel shared(done)
  {
    HashMap *map = createHashMap(read_size, 0);
    unsigned long long thread_read_section = read_size * omp_get_thread_num();
    unsigned long long thread_read_section_end = read_size == 0 ? end : min(thread_read_section + read_size, end);

    FILE *fp_local = fopen(argv[1], "r");
#pragma omp barrier
    double reader_start = omp_get_wtime();
    Queue *q = ProcessWord(fp_local, thread_read_section, thread_read_section_end, size_per_thread);
#pragma omp barrier
    double reader_end = omp_get_wtime();


    fclose(fp_local);
#pragma omp atomic
    done += 1;
#pragma omp flush(done)
    Task *batch[BATCH_SIZE];
    int got;

#pragma omp barrier
    double mapper_start = omp_get_wtime();


    while (1)
    {
      got = Dequeue(q, batch, size_per_thread);
      if (got == 0)
      {
#pragma omp flush(done)
        if (done == nt)
          break;
        continue;
      }
#pragma omp for 
      for (int i = 0; i < got; i++)
      {
        Task *t = batch[i];
        // process task
        unsigned int hashValue = hash(t->word, t->len, read_size);
        if (map->buckets[hashValue] == 0)
        {
          map->buckets[hashValue] += 1;
          map->iarr[hashValue] = strdup(t->word);
        }
        else if (strcmp(map->iarr[hashValue], t->word) == 0)
          map->buckets[hashValue] += 1;

        else
        {
          int idx = hashValue;
          while (idx < read_size && map->iarr[idx] != 0 && strcmp(map->iarr[idx], t->word) != 0)
            idx++;
          map->buckets[idx] += 1;
          map->iarr[idx] = strdup(t->word);
        }

        free(t->word);
        free(t);
      }
    }
#pragma omp barrier
    double mapper_end = omp_get_wtime();



    double agg_start = omp_get_wtime();
  #pragma omp parallel for schedule(dynamic)
    for (int i = 0; i < read_size; i++)
    {
      if (map->iarr[i] == NULL) continue;

      const char *word = map->iarr[i];
      int word_count = map->buckets[i];

      int cur = hash(word, strlen(word), TABLE_SIZE);

      while (1)
      {
        int lock_idx = cur % nt;            // which lock protects this slot
        omp_set_lock(&global_map->locks[lock_idx]);

        if (global_map->iarr[cur] == NULL)
        {
          // free/clone as needed; we own this slot now
          global_map->iarr[cur] = strdup(word);
          global_map->buckets[cur] = word_count;
          global_map->unique_word_count += 1;
          global_map->total_word_count += word_count;
          omp_unset_lock(&global_map->locks[lock_idx]);
          break;
        }
        else if (strcmp(global_map->iarr[cur], word) == 0)
        {
          global_map->buckets[cur] += word_count;
          global_map->total_word_count += word_count;
          omp_unset_lock(&global_map->locks[lock_idx]);
          break;
        }
        else
        {
          // not a match: release this slot and try next
          omp_unset_lock(&global_map->locks[lock_idx]);
          cur = (cur + 1) % TABLE_SIZE;
          // loop and lock the next slot before inspecting it
        }
      }
    }

#pragma omp barrier
    double agg_end = omp_get_wtime();

    end_time = omp_get_wtime();
#pragma omp single
    {

      printf("Reader. Time: %f\n", reader_end - reader_start);
      printf("Mapper. Time: %f\n", mapper_end - mapper_start);
      printf("Aggr. Time: %f\n", agg_end - agg_start);

    }
  }

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
  printf("File: %s\nThreads: %d\nTotal Time:  %f\nTotal: %d Unique: %d\nBatch Size: %d\n\n", argv[1], nt, total_time, global_map->total_word_count, global_map->unique_word_count, BATCH_SIZE);
  fflush(stdout);
  return 0;
}
/* --------------------------Main function-------------------------- */
