/* ui.c
 *
 * UI simple que se comunica con el daemon por FIFOs usando Request/Response (common.h).
 * Menú con 4 opciones:
 * 1) Ingresar primer criterio de búsqueda (title)
 * 2) Ingresar segundo criterio de búsqueda (date)
 * 3) Realizar búsqueda
 * 4) Salir
 *
 * Comportamiento:
 * - Al elegir 1 se muestra: "Ingrese primer criterio de búsqueda (title):"
 * - Tras ingresar el primer criterio se vuelve a mostrar el menú con la línea 1
 *   mostrando: "1. Ingresar primer criterio de búsqueda (title): <criterio ingresado por el usuario>"
 * - Después de realizar la búsqueda se imprime el resultado y se muestra nuevamente el menú.
 *
 * Compilar:
 *   gcc -std=c11 -O2 -o ui ui.c
 *
 * Nota: requiere common.h con las definiciones de Request, Response, FIFO_REQ, FIFO_RES.
 */

#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <errno.h>
#include <time.h>

#include "common.h" /* Debe definir Request, Response, FIFO_REQ, FIFO_RES */

#define MAX_INPUT 1024

/* Trim in-place both ends */
static void trim_inplace(char *s) {
    if (!s) return;
    char *a = s;
    while (*a && isspace((unsigned char)*a)) a++;
    if (a != s) memmove(s, a, strlen(a)+1);
    size_t n = strlen(s);
    while (n > 0 && isspace((unsigned char)s[n-1])) { s[n-1] = '\0'; n--; }
}

/* Print the menu with header/footer */
static void print_menu(const char *title_value, const char *date_value) {
    printf("====== BUSCADOR DE PAPERS DE INVESTIGACIÓN ======\n");
    if (title_value && title_value[0] != '\0') {
        printf("1. Ingresar primer criterio de búsqueda (title): %s\n", title_value);
    } else {
        printf("1. Ingresar primer criterio de búsqueda (title)\n");
    }
    if (date_value && date_value[0] != '\0') {
        printf("2. Ingresar segundo criterio de búsqueda (date): %s\n", date_value);
    } else {
        printf("2. Ingresar segundo criterio de búsqueda (date)\n");
    }
    printf("3. Realizar búsqueda\n");
    printf("4. Salir\n");
    printf("=================================================\n");
    printf("Elija una opción: ");
    fflush(stdout);
}

/* Validate YYYY-MM-DD including month/day ranges and leap years.
 * Returns 1 if valid, 0 otherwise.
 */
static int is_leap_year(int y) {
    if (y % 4 != 0) return 0;
    if (y % 100 != 0) return 1;
    if (y % 400 != 0) return 0;
    return 1;
}

static int valid_ymd_format(const char *s) {
    if (!s) return 0;
    if (strlen(s) != 10) return 0;
    if (s[4] != '-' || s[7] != '-') return 0;
    for (int i = 0; i < 10; ++i) {
        if (i == 4 || i == 7) continue;
        if (!isdigit((unsigned char)s[i])) return 0;
    }
    /* parse */
    char year_s[5] = {0}, mon_s[3] = {0}, day_s[3] = {0};
    memcpy(year_s, s, 4);
    memcpy(mon_s, s+5, 2);
    memcpy(day_s, s+8, 2);
    int y = atoi(year_s);
    int m = atoi(mon_s);
    int d = atoi(day_s);
    if (m < 1 || m > 12) return 0;
    if (d < 1) return 0;
    int mdays = 31;
    if (m == 2) {
        mdays = is_leap_year(y) ? 29 : 28;
    } else if (m==4 || m==6 || m==9 || m==11) {
        mdays = 30;
    }
    if (d > mdays) return 0;
    return 1;
}

/* Send Request (struct) to FIFO_REQ and read Response from FIFO_RES.
 * Returns 0 on success (response filled), -1 on error.
 */
static int send_request_and_get_response(const Request *req, Response *res) {
    if (!req || !res) return -1;

    /* Ensure FIFOs exist (try to create if missing) */
    if (access(FIFO_REQ, F_OK) != 0) {
        if (mkfifo(FIFO_REQ, 0666) == -1 && errno != EEXIST) {
            perror("mkfifo FIFO_REQ");
            return -1;
        }
    }
    if (access(FIFO_RES, F_OK) != 0) {
        if (mkfifo(FIFO_RES, 0666) == -1 && errno != EEXIST) {
            perror("mkfifo FIFO_RES");
            return -1;
        }
    }

    /* Open request FIFO for writing (blocks until reader opens) */
    int fdw = open(FIFO_REQ, O_WRONLY);
    if (fdw < 0) {
        perror("open FIFO_REQ for write");
        return -1;
    }

    ssize_t w = write(fdw, req, sizeof(*req));
    if (w != (ssize_t)sizeof(*req)) {
        if (w < 0) perror("write FIFO_REQ");
        close(fdw);
        return -1;
    }
    close(fdw);

    /* Now open response FIFO for reading (blocks until writer opens) */
    int fdr = open(FIFO_RES, O_RDONLY);
    if (fdr < 0) {
        perror("open FIFO_RES for read");
        return -1;
    }

    ssize_t r = read(fdr, res, sizeof(*res));
    close(fdr);
    if (r != (ssize_t)sizeof(*res)) {
        if (r < 0) perror("read FIFO_RES");
        return -1;
    }

    return 0;
}

int main(void) {
    char title_buf[256] = {0};   /* coincide con common.h.value1 length (256) */
    char date_buf[256]  = {0};   /* coincide con common.h.value2 length (256) */

    while (1) {
        print_menu(title_buf, date_buf);

        char opt_line[64];
        if (!fgets(opt_line, sizeof(opt_line), stdin)) {
            printf("\nSaliendo...\n");
            break;
        }
        trim_inplace(opt_line);
        if (opt_line[0] == '\0') continue;

        int opt = atoi(opt_line);
        if (opt == 1) {
            printf("Ingrese primer criterio de búsqueda (title): ");
            fflush(stdout);
            char input[MAX_INPUT];
            if (!fgets(input, sizeof(input), stdin)) {
                printf("\nEntrada interrumpida. Volviendo al menú.\n");
                continue;
            }
            input[strcspn(input, "\r\n")] = '\0';
            trim_inplace(input);
            /* store into title_buf safely */
            strncpy(title_buf, input, sizeof(title_buf)-1);
            title_buf[sizeof(title_buf)-1] = '\0';
            continue;
        } else if (opt == 2) {
            printf("Ingrese segundo criterio de búsqueda (date): ");
            fflush(stdout);
            char input[MAX_INPUT];
            if (!fgets(input, sizeof(input), stdin)) {
                printf("\nEntrada interrumpida. Volviendo al menú.\n");
                continue;
            }
            input[strcspn(input, "\r\n")] = '\0';
            trim_inplace(input);

            if (input[0] != '\0') {
                if (!valid_ymd_format(input)) {
                    printf("Formato de fecha inválido. Use YYYY-MM-DD. Volviendo al menú.\n");
                    continue;
                }
            }
            /* store valid date (or empty) */
            strncpy(date_buf, input, sizeof(date_buf)-1);
            date_buf[sizeof(date_buf)-1] = '\0';
            continue;
        } else if (opt == 3) {
            Request req;
            Response res;
            memset(&req, 0, sizeof(req));
            memset(&res, 0, sizeof(res));

            if (title_buf[0] != '\0') {
                snprintf(req.field_name1, sizeof(req.field_name1), "%s", "title");
                snprintf(req.value1, sizeof(req.value1), "%s", title_buf);
            } else {
                req.field_name1[0] = '\0';
                req.value1[0] = '\0';
            }

            if (date_buf[0] != '\0') {
                snprintf(req.field_name2, sizeof(req.field_name2), "%s", "update_date");
                snprintf(req.value2, sizeof(req.value2), "%s", date_buf);
            } else {
                req.field_name2[0] = '\0';
                req.value2[0] = '\0';
            }

            printf("Realizando búsqueda...\n");
            fflush(stdout);

            struct timespec t1, t2;
            clock_gettime(CLOCK_MONOTONIC, &t1);

            int ok = send_request_and_get_response(&req, &res);

            clock_gettime(CLOCK_MONOTONIC, &t2);
            double elapsed = (t2.tv_sec - t1.tv_sec) + (t2.tv_nsec - t1.tv_nsec) / 1e9;

            if (ok != 0) {
                printf("Error comunicándose con el buscador. Asegúrate de que el daemon está corriendo.\n");
                continue;
            }

            printf(">> Tiempo que tardó la búsqueda: %.3f segundos\n", elapsed);
            printf(">> Resultado de la búsqueda:\n");

            if (res.result[0] == '\0') {
                printf("\n");
            } else {
                size_t len = strnlen(res.result, sizeof(res.result));
                if (len > 0) {
                    fwrite(res.result, 1, len, stdout);
                    if (res.result[len-1] != '\n') putchar('\n');
                } else {
                    putchar('\n');
                }
            }

            continue;
        } else if (opt == 4) {
            printf("Saliendo...\n");
            break;
        } else {
            printf("Opción no válida. Intenta de nuevo.\n");
            continue;
        }
    }

    return 0;
}
