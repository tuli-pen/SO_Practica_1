#ifndef INDEX_H
#define INDEX_H

#include <stdio.h>

#define N_BUCKETS 1000      /* usamos módulo 1000 */
#define KEY_SIZE 256        /* títulos largos */

/* Estructuras que se guardan en disco */
typedef struct {
    int n_buckets;
    long offset_buckets;
    long offset_entries;
} IndexHeader;

typedef struct {
    long first_entry_offset;    /* -1 si el bucket está vacío */
} BucketDisk;

typedef struct {
    char key[KEY_SIZE];         /* título del paper */
    long csv_offset;            /* posición del registro en el CSV */
    long next_entry;            /* offset al siguiente EntryDisk */
} EntryDisk;

/* Prototipos públicos */
// index.h
int build_index(const char *csv_path, const char *index_path);
long search_in_index(const char *key, const char *index_path);

#endif

