#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>   /* strcasecmp, strncasecmp */
#include <ctype.h>
#include <time.h>
#include "index.h"
#include "hash.h"

#define RANGE 12  // rango para búsqueda parcial

/* Implementación portable de búsqueda case-insensitive de subcadena.
 * Devuelve puntero a la primera ocurrencia o NULL.
 * Usa strncasecmp para comparar ventanas de tamaño needle_len.
 */
static char *ci_strcasestr(const char *haystack, const char *needle) {
    if (!haystack || !needle) return NULL;
    size_t needle_len = strlen(needle);
    if (needle_len == 0) return (char *)haystack;
    for (const char *p = haystack; *p; ++p) {
        size_t rem = strlen(p);
        if (rem < needle_len) return NULL;
        if (strncasecmp(p, needle, needle_len) == 0) return (char *)p;
    }
    return NULL;
}

// --- Función para limpiar texto ---
void limpiar_texto(char *s) {
    if (!s) return;
    size_t len = strlen(s);
    if (len == 0) return;

    if (s[0] == '"') {
        memmove(s, s + 1, len);
        s[len-1] = '\0';
        len = strlen(s);
    }
    if (len > 0 && s[len - 1] == '"') s[len - 1] = '\0';

    len = strlen(s);
    while (len > 0 && isspace((unsigned char)s[len - 1])) {
        s[len - 1] = '\0';
        len--;
    }
}

// --- Función que construye el índice si no existe ---
int build_index(const char *csv_path, const char *index_path) {
    FILE *csv = fopen(csv_path, "r");
    if (!csv) { perror("Error abriendo CSV"); return -1; }

    FILE *idx = fopen(index_path, "wb+");
    if (!idx) { perror("Error creando índice"); fclose(csv); return -1; }

    // --- Header ---
    IndexHeader header = { N_BUCKETS, sizeof(IndexHeader), sizeof(IndexHeader) + sizeof(BucketDisk) * N_BUCKETS };
    if (fwrite(&header, sizeof(IndexHeader), 1, idx) != 1) {
        perror("Error escribiendo header índice");
        fclose(csv); fclose(idx);
        return -1;
    }

    // --- Buckets vacíos ---
    BucketDisk empty = { .first_entry_offset = -1 };
    for (int i = 0; i < N_BUCKETS; i++) {
        if (fwrite(&empty, sizeof(BucketDisk), 1, idx) != 1) {
            perror("Error escribiendo buckets iniciales");
            fclose(csv); fclose(idx);
            return -1;
        }
    }

    // --- Leer CSV ---
    char line[4096];
    long line_start;
    /* comprobar retorno al saltar encabezado */
    if (!fgets(line, sizeof(line), csv)) {
        /* archivo vacío o error */
        if (feof(csv)) {
            fprintf(stderr, "CSV vacío o sin encabezado: %s\n", csv_path);
        } else {
            perror("Error leyendo encabezado CSV");
        }
        fclose(csv); fclose(idx);
        return -1;
    }

    while ( (line_start = ftell(csv)), fgets(line, sizeof(line), csv) ) {
        /* strtok aquí es simple; si el CSV puede tener comas dentro de campos con comillas
           deberías usar un parseador robusto. Aquí mantenemos la aproximación original. */
        char *token = strtok(line, ",\n\r");
        char *key = NULL;
        int col = 1;
        while (token) {
            if (col == 4) { key = token; break; }
            token = strtok(NULL, ",\n\r");
            col++;
        }
        if (!key) continue;
        limpiar_texto(key);

        unsigned long h = hash_string(key) % N_BUCKETS;
        long bucket_offset = sizeof(IndexHeader) + sizeof(BucketDisk) * h;

        BucketDisk b;
        if (fseek(idx, bucket_offset, SEEK_SET) != 0) {
            perror("fseek bucket (build_index)");
            continue;
        }
        if (fread(&b, sizeof(BucketDisk), 1, idx) != 1) {
            /* Si no pudimos leer el bucket (raro), inicializamos como vacío */
            b.first_entry_offset = -1;
            /* no hacemos continue; intentaremos insertar igualmente */
        }

        EntryDisk entry;
        memset(&entry, 0, sizeof(EntryDisk));
        strncpy(entry.key, key, KEY_SIZE - 1);
        entry.csv_offset = line_start;
        entry.next_entry = b.first_entry_offset;

        if (fseek(idx, 0, SEEK_END) != 0) {
            perror("fseek SEEK_END (build_index)");
            continue;
        }
        long new_entry_offset = ftell(idx);
        if (fwrite(&entry, sizeof(EntryDisk), 1, idx) != 1) {
            perror("fwrite entry (build_index)");
            continue;
        }

        b.first_entry_offset = new_entry_offset;
        if (fseek(idx, bucket_offset, SEEK_SET) != 0) {
            perror("fseek bucket write (build_index)");
            continue;
        }
        if (fwrite(&b, sizeof(BucketDisk), 1, idx) != 1) {
            perror("fwrite bucket (build_index)");
            continue;
        }
    }

    if (ferror(csv)) {
        perror("Error leyendo CSV durante build_index");
        /* No abortamos necesariamente; ya se escribió lo que se pudo */
    }

    fclose(csv);
    fclose(idx);
    printf("Índice generado correctamente con %d buckets.\n", N_BUCKETS);
    return 0;
}

// --- Función de búsqueda híbrida ---
void search_by_keyword(const char *keyword, int exact, const char *index_file) {
    if (!keyword) return;

    // Crear índice si no existe
    FILE *test = fopen(index_file, "rb");
    if (!test) {
        printf("No existe '%s', creando índice...\n", index_file);
        if (build_index("arxiv.csv", index_file) == -1) return;
    } else fclose(test);

    FILE *idx = fopen(index_file, "rb");
    FILE *csv = fopen("arxiv.csv", "r");
    if (!idx || !csv) {
        perror("Error abriendo archivos");
        if (idx) fclose(idx);
        if (csv) fclose(csv);
        return;
    }

    clock_t start = clock();
    unsigned long h = hash_string(keyword) % N_BUCKETS;
    int found = 0;

    int offset_start = exact ? 0 : -RANGE;
    int offset_end   = exact ? 0 : RANGE;

    for (int offset = offset_start; offset <= offset_end; offset++) {
        int bucket_index = (int)h + offset;
        if (bucket_index < 0 || bucket_index >= N_BUCKETS) continue;

        long bucket_offset = sizeof(IndexHeader) + sizeof(BucketDisk) * bucket_index;
        BucketDisk b;
        if (fseek(idx, bucket_offset, SEEK_SET) != 0) {
            /* error seeking; saltar bucket */
            continue;
        }
        if (fread(&b, sizeof(BucketDisk), 1, idx) != 1) {
            /* no se pudo leer el bucket; saltar */
            continue;
        }

        long current = b.first_entry_offset;
        EntryDisk entry;

        while (current != -1) {
            if (fseek(idx, current, SEEK_SET) != 0) {
                /* error de lectura; salir del while de esta bucket */
                break;
            }
            if (fread(&entry, sizeof(EntryDisk), 1, idx) != 1) {
                /* no se pudo leer la entrada; romper */
                break;
            }

            int match = 0;
            if (exact) {
                if (strcasecmp(entry.key, keyword) == 0) match = 1;
            } else {
                if (ci_strcasestr(entry.key, keyword)) match = 1;
            }

            if (match) {
                found++;

                if (fseek(csv, entry.csv_offset, SEEK_SET) == 0) {
                    char line[4096];
                    if (fgets(line, sizeof(line), csv)) {
                        printf("%s", line);
                    } else {
                        /* no se pudo leer la línea en csv (posible error) */
                        clearerr(csv);
                    }
                }

                if (found >= 50) {
                    printf("\nMostrando solo las primeras 50 coincidencias.\n");
                    goto FIN;
                }
            }

            current = entry.next_entry;
        }
    }

FIN:
    clock_t end = clock();
    double segundos = (double)(end - start) / CLOCKS_PER_SEC;

    if (!found)
        printf("No se encontraron resultados con '%s'\n", keyword);
    else
        printf("\nTotal encontrados: %d\n", found);

    printf("Tiempo de búsqueda: %.3f segundos\n", segundos);

    fclose(idx);
    fclose(csv);
}
