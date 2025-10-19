/* search_worker.c
 *
 * Worker que se comunica binariamente con la UI mediante Request/Response (common.h).
 * Búsqueda por SUBCADENA (case-insensitive) en title + filtro opcional update_date (col 12).
 *
 * Requisitos:
 *  - common.h (FIFO_REQ, FIFO_RES, Request, Response)
 *  - index.h (IndexHeader, BucketDisk, EntryDisk, KEY_SIZE, N_BUCKETS)
 *  - hash.h / hash.c (hash_string)
 *  - build_index(...) en index2.c
 *
 * Compilar ejemplo:
 *  gcc -std=c11 -O2 -o search_worker search_worker.c index2.c hash.c
 */

#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>   /* strcasecmp, strncasecmp */
#include <ctype.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <errno.h>
#include <time.h>

#include "common.h"    /* FIFO_REQ, FIFO_RES, Request, Response */
#include "index.h"
#include "hash.h"

#ifndef KEY_SIZE
#define KEY_SIZE 256
#endif

#define CSV_FILE "arxiv.csv"
#define INDEX_FILE "index.bin"
#define MAX_LINE 8192
#define MAX_RESULTS 50
#define BUCKET_RANGE 12    /* heurística: escanear vecinos alrededor del bucket */

int build_index(const char *csv_path, const char *index_path);

/* Portable case-insensitive substring search */
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

/* trim both ends */
static void trim_inplace(char *s) {
    if (!s) return;
    char *a = s;
    while (*a && isspace((unsigned char)*a)) a++;
    if (a != s) memmove(s, a, strlen(a)+1);
    size_t n = strlen(s);
    while (n > 0 && isspace((unsigned char)s[n-1])) { s[n-1] = '\0'; n--; }
}

/* Parse CSV column (1-based) with basic quote support */
static int csv_get_column(const char *line, int target_col, char *out, size_t out_sz) {
    int col = 1;
    const char *p = line;
    while (*p && *p != '\n' && *p != '\r') {
        if (col == target_col) {
            if (*p == '"') {
                p++; /* skip " */
                size_t pos = 0;
                while (*p) {
                    if (*p == '"') {
                        if (*(p+1) == '"') {
                            if (pos + 1 < out_sz) out[pos++] = '"';
                            p += 2;
                            continue;
                        } else { p++; break; } /* end quote */
                    }
                    if (pos + 1 < out_sz) out[pos++] = *p;
                    p++;
                }
                out[pos] = '\0';
                trim_inplace(out);
                return 1;
            } else {
                const char *start = p;
                while (*p && *p != ',' && *p != '\n' && *p != '\r') p++;
                size_t len = (size_t)(p - start);
                if (len >= out_sz) len = out_sz - 1;
                memcpy(out, start, len);
                out[len] = '\0';
                trim_inplace(out);
                return 1;
            }
        }

        /* advance to next field */
        if (*p == '"') {
            p++;
            while (*p) {
                if (*p == '"' && *(p+1) != '"') { p++; break; }
                if (*p == '"' && *(p+1) == '"') p += 2;
                else p++;
            }
            if (*p == ',') { p++; col++; }
        } else {
            while (*p && *p != ',' && *p != '\n' && *p != '\r') p++;
            if (*p == ',') { p++; col++; }
        }
    }
    out[0] = '\0';
    return 0;
}

/* Field name match ignoring case/spaces (small helper) */
static int field_is(const char *field, const char *target) {
    if (!field || !target) return 0;
    char a[128], b[128];
    strncpy(a, field, sizeof(a)-1); a[sizeof(a)-1] = '\0'; trim_inplace(a);
    strncpy(b, target, sizeof(b)-1); b[sizeof(b)-1] = '\0'; trim_inplace(b);
    return strcasecmp(a, b) == 0;
}

/* Search across buckets in a neighbor range using substring match for title.
 * Append matching CSV lines into resp_buf (size resp_sz). Returns number of matches
 * found (>0), 0 if none, -1 on error.
 */
static int search_by_title_and_update(const char *title_value, const char *update_value,
                                      char *resp_buf, size_t resp_sz) {
    if (!title_value || resp_buf == NULL) return -1;

    /* Ensure index exists */
    if (access(INDEX_FILE, F_OK) != 0) {
        if (build_index(CSV_FILE, INDEX_FILE) != 0) return -1;
    }

    FILE *idx = fopen(INDEX_FILE, "rb");
    FILE *csv = fopen(CSV_FILE, "r");
    if (!idx || !csv) {
        if (idx) fclose(idx);
        if (csv) fclose(csv);
        return -1;
    }

    /* read header */
    IndexHeader header;
    if (fseek(idx, 0, SEEK_SET) != 0 || fread(&header, sizeof(IndexHeader), 1, idx) != 1) {
        fclose(idx); fclose(csv);
        return -1;
    }
    long n_buckets = header.n_buckets;
    if (n_buckets <= 0) n_buckets = N_BUCKETS;

    unsigned long h = hash_string(title_value) % (unsigned long)n_buckets;

    int found = 0;
    size_t used = 0;
    resp_buf[0] = '\0';

    int off_start = -BUCKET_RANGE;
    int off_end   = BUCKET_RANGE;

    char linebuf[MAX_LINE];

    for (int off = off_start; off <= off_end && found < MAX_RESULTS; ++off) {
        long bucket_idx = (long)h + off;
        if (bucket_idx < 0 || bucket_idx >= n_buckets) continue;

        long bucket_offset = sizeof(IndexHeader) + sizeof(BucketDisk) * bucket_idx;
        BucketDisk b;
        if (fseek(idx, bucket_offset, SEEK_SET) != 0) continue;
        if (fread(&b, sizeof(BucketDisk), 1, idx) != 1) continue;

        long current = b.first_entry_offset;
        EntryDisk entry;

        while (current != -1 && found < MAX_RESULTS) {
            if (fseek(idx, current, SEEK_SET) != 0) break;
            if (fread(&entry, sizeof(EntryDisk), 1, idx) != 1) break;

            /* substring match (case-insensitive) */
            if (ci_strcasestr(entry.key, title_value)) {
                /* read CSV line at offset */
                if (fseek(csv, entry.csv_offset, SEEK_SET) == 0) {
                    if (fgets(linebuf, sizeof(linebuf), csv)) {
                        int pass_update = 1;
                        if (update_value && update_value[0] != '\0') {
                            char parsed_update[64];
                            if (!csv_get_column(linebuf, 12, parsed_update, sizeof(parsed_update))) {
                                pass_update = 0;
                            } else {
                                if (strcasecmp(parsed_update, update_value) != 0) pass_update = 0;
                            }
                        }
                        if (pass_update) {
                            /* append linebuf to resp_buf if space permits */
                            size_t line_len = strnlen(linebuf, sizeof(linebuf));
                            /* ensure space for at least one char and terminating null */
                            if (used + line_len + 1 < resp_sz) {
                                memcpy(resp_buf + used, linebuf, line_len);
                                used += line_len;
                                resp_buf[used] = '\0';
                                found++;
                            } else {
                                /* not enough space left; stop collecting */
                                goto FINISH_SEARCH;
                            }
                        }
                    } /* fgets */
                } /* fseek */
            } /* if substring */

            current = entry.next_entry;
        } /* while entries */

    } /* for buckets */

FINISH_SEARCH:
    fclose(idx);
    fclose(csv);
    return found;
}

int main(void) {
    for (;;) {
        Request req;
        Response res;
        memset(&req, 0, sizeof(req));
        memset(&res, 0, sizeof(res));

        /* Blocking open/read of request FIFO */
        int fd_in = open(FIFO_REQ, O_RDONLY);
        if (fd_in < 0) {
            if (errno == ENOENT) {
                if (mkfifo(FIFO_REQ, 0666) == -1 && errno != EEXIST) {
                    perror("mkfifo FIFO_REQ");
                    sleep(1);
                    continue;
                }
                fd_in = open(FIFO_REQ, O_RDONLY);
                if (fd_in < 0) { perror("open FIFO_REQ"); sleep(1); continue; }
            } else {
                perror("open FIFO_REQ");
                sleep(1);
                continue;
            }
        }

        ssize_t rr = read(fd_in, &req, sizeof(req));
        close(fd_in);
        if (rr != (ssize_t)sizeof(req)) {
            /* ignore bad/partial reads and wait for next request */
            continue;
        }

        /* Extract title and update_date values (supports either field position) */
        char title_val[KEY_SIZE]; title_val[0] = '\0';
        char update_val[64]; update_val[0] = '\0';

        if (field_is(req.field_name1, "title")) {
            strncpy(title_val, req.value1, sizeof(title_val)-1);
            title_val[sizeof(title_val)-1] = '\0';
            trim_inplace(title_val);
        } else if (field_is(req.field_name2, "title")) {
            strncpy(title_val, req.value2, sizeof(title_val)-1);
            title_val[sizeof(title_val)-1] = '\0';
            trim_inplace(title_val);
        }

        if (field_is(req.field_name1, "update_date") || field_is(req.field_name1, "updatedate") || field_is(req.field_name1, "update-date")) {
            strncpy(update_val, req.value1, sizeof(update_val)-1);
            update_val[sizeof(update_val)-1] = '\0';
            trim_inplace(update_val);
        } else if (field_is(req.field_name2, "update_date") || field_is(req.field_name2, "updatedate") || field_is(req.field_name2, "update-date")) {
            strncpy(update_val, req.value2, sizeof(update_val)-1);
            update_val[sizeof(update_val)-1] = '\0';
            trim_inplace(update_val);
        }

        /* If no title provided -> UI expects NA */
        if (title_val[0] == '\0') {
            strncpy(res.result, "NA", sizeof(res.result)-1);
        } else {
            char buf[sizeof(res.result)];
            memset(buf, 0, sizeof(buf));
            int found = search_by_title_and_update(title_val, (update_val[0] ? update_val : NULL),
                                                  buf, sizeof(buf));
            if (found <= 0) {
                strncpy(res.result, "NA", sizeof(res.result)-1);
            } else {
                /* copy collected results into res.result (already fits) */
                strncpy(res.result, buf, sizeof(res.result)-1);
                res.result[sizeof(res.result)-1] = '\0';
            }
        }

        /* Write response (blocking until UI reads) */
        int fd_out = open(FIFO_RES, O_WRONLY);
        if (fd_out < 0) {
            if (errno == ENOENT) {
                if (mkfifo(FIFO_RES, 0666) == -1 && errno != EEXIST) {
                    perror("mkfifo FIFO_RES");
                    continue;
                }
                fd_out = open(FIFO_RES, O_WRONLY);
                if (fd_out < 0) { perror("open FIFO_RES"); continue; }
            } else {
                perror("open FIFO_RES");
                continue;
            }
        }

        ssize_t ww = write(fd_out, &res, sizeof(res));
        (void)ww; /* ignore partial-write complexities (UI expects sizeof) */
        close(fd_out);
    }

    return 0;
}
