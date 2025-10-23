/* ui.c
 * UI que se comunica con un daemon vía FIFOs (IPC). Usa structs Request/Response definidos en common.h.
 * Menú: (1) title  (2) date (YYYY-MM-DD)  (3) buscar  (4) salir
 * NOTA: la UI NO hace la búsqueda; sólo valida entradas, arma la Request, mide tiempo y muestra la Response.
 */

#define _POSIX_C_SOURCE 200809L  // Habilita APIs POSIX como mkfifo(3) y clock_gettime(2) en headers estándar.

#include <stdio.h>               // printf, fgets, fwrite, perror (vía stdio)
#include <stdlib.h>              // atoi, exit, size_t
#include <string.h>              // strlen, strncpy, memcpy, memmove, memset, strnlen, strcmp
#include <ctype.h>               // isspace, isdigit (para validar y limpiar entrada)
#include <unistd.h>              // access, close, read, write
#include <fcntl.h>               // open y flags O_*
#include <sys/stat.h>            // mkfifo, permisos 0666
#include <errno.h>               // errno para diagnosticar errores de syscalls
#include <time.h>                // clock_gettime, struct timespec, CLOCK_MONOTONIC

#include "common.h"              // Declara Request, Response y rutas FIFO_REQ/FIFO_RES (protocolo UI<->daemon)

#define MAX_INPUT 1024           // Límite de lectura por línea desde stdin (defensa ante entradas largas)

/* -------------------------------------------------------------------------- */
/* trim_inplace: elimina espacios en blanco (y \r/\n si quedaron) al inicio y al final del string s */
static void trim_inplace(char *s) {
    if (!s) return;                                      // Si s==NULL, no hay nada que hacer.
    char *a = s;                                         // Puntero auxiliar para avanzar el inicio efectivo.
    while (*a && isspace((unsigned char)*a)) a++;        // Salta todos los espacios iniciales.
    if (a != s) memmove(s, a, strlen(a)+1);              // Compacta hacia el inicio si corrió el puntero.
    size_t n = strlen(s);                                // Longitud actual del string ya compactado.
    while (n > 0 && isspace((unsigned char)s[n-1])) {    // Retrocede cortando espacios finales.
        s[n-1] = '\0';                                   // Reemplaza por terminador.
        n--;                                             // Decrementa longitud lógica.
    }
}

/* -------------------------------------------------------------------------- */
/* print_menu: imprime el menú con los valores actuales de title/date si existen */
static void print_menu(const char *title_value, const char *date_value) {
    printf("====== BUSCADOR DE PAPERS DE INVESTIGACIÓN ======\n");       // Encabezado visual.
    if (title_value && title_value[0] != '\0') {                        // Si ya hay title capturado...
        printf("1. Ingresar primer criterio de búsqueda (title): %s\n", title_value); // Lo muestra en línea.
    } else {
        printf("1. Ingresar primer criterio de búsqueda (title)\n");    // Sino, muestra la opción “vacía”.
    }
    if (date_value && date_value[0] != '\0') {                          // Si ya hay date capturado...
        printf("2. Ingresar segundo criterio de búsqueda (date): %s\n", date_value);  // Lo muestra en línea.
    } else {
        printf("2. Ingresar segundo criterio de búsqueda (date)\n");    // Sino, opción “vacía”.
    }
    printf("3. Realizar búsqueda\n");                                   // Dispara el envío al daemon.
    printf("4. Salir\n");                                               // Termina el programa.
    printf("=================================================\n");       // Separador estético.
    printf("Elija una opción: ");                                       // Prompt de lectura de opción.
    fflush(stdout);                                                     // Garantiza que el prompt se imprima ya.
}

/* -------------------------------------------------------------------------- */
/* is_leap_year: determina si el año es bisiesto */
static int is_leap_year(int y) {
    if (y % 4 != 0) return 0;             // No múltiplo de 4 -> no bisiesto.
    if (y % 100 != 0) return 1;           // Múltiplo de 4 y no de 100 -> bisiesto.
    if (y % 400 != 0) return 0;           // Múltiplo de 100 pero no de 400 -> no bisiesto.
    return 1;                             // Múltiplo de 400 -> bisiesto.
}

/* -------------------------------------------------------------------------- */
/* valid_ymd_format: valida formato "YYYY-MM-DD" y rangos (mes, día, bisiesto en feb) */
static int valid_ymd_format(const char *s) {
    if (!s) return 0;                                   // Null -> inválida.
    if (strlen(s) != 10) return 0;                      // Debe tener exactamente 10 chars.
    if (s[4] != '-' || s[7] != '-') return 0;           // Guiones en posiciones 4 y 7.
    for (int i = 0; i < 10; ++i) {                      // Recorre los 10 chars...
        if (i == 4 || i == 7) continue;                 // ... ignorando los guiones ...
        if (!isdigit((unsigned char)s[i])) return 0;    // ... el resto deben ser dígitos.
    }
    char year_s[5] = {0}, mon_s[3] = {0}, day_s[3] = {0}; // Buffers para YYYY, MM, DD (con terminador).
    memcpy(year_s, s, 4);                               // Copia YYYY.
    memcpy(mon_s, s+5, 2);                              // Copia MM.
    memcpy(day_s, s+8, 2);                              // Copia DD.
    int y = atoi(year_s);                               // Convierte año a int.
    int m = atoi(mon_s);                                // Convierte mes a int.
    int d = atoi(day_s);                                // Convierte día a int.
    if (m < 1 || m > 12) return 0;                      // Mes fuera de 1..12 -> inválido.
    if (d < 1) return 0;                                // Día mínimo 1.
    int mdays = 31;                                     // Por defecto, 31 días.
    if (m == 2) {                                       // Febrero:
        mdays = is_leap_year(y) ? 29 : 28;              // 29 si bisiesto; 28 si no.
    } else if (m==4 || m==6 || m==9 || m==11) {         // Meses de 30 días:
        mdays = 30;
    }
    if (d > mdays) return 0;                            // Día excede máximo -> inválido.
    return 1;                                           // Todo OK.
}

/* -------------------------------------------------------------------------- */
/* send_request_and_get_response:
 * - Asegura que existan los FIFOs (mkfifo si faltan).
 * - Abre FIFO_REQ para escribir la Request (bloquea hasta que daemon lea).
 * - Abre FIFO_RES para leer la Response (bloquea hasta que daemon escriba).
 * Devuelve 0 si éxito; -1 si falla cualquier paso (y deja perror para diagnóstico).
 */
static int send_request_and_get_response(const Request *req, Response *res) {
    if (!req || !res) return -1;                        // Validación de punteros.

    // Verifica que FIFO_REQ exista; si no, intenta crearlo (0666: lectura/escritura para todos).
    if (access(FIFO_REQ, F_OK) != 0) {                  // access comprueba existencia del path.
        if (mkfifo(FIFO_REQ, 0666) == -1 && errno != EEXIST) {
            perror("mkfifo FIFO_REQ");                  // Error real creando FIFO_REQ.
            return -1;
        }
    }
    // Verifica que FIFO_RES exista; si no, intenta crearlo.
    if (access(FIFO_RES, F_OK) != 0) {
        if (mkfifo(FIFO_RES, 0666) == -1 && errno != EEXIST) {
            perror("mkfifo FIFO_RES");                  // Error real creando FIFO_RES.
            return -1;
        }
    }

    // Abre FIFO_REQ sólo para ESCRITURA; se bloqueará hasta que el daemon lo abra en LECTURA.
    int fdw = open(FIFO_REQ, O_WRONLY);                 // Devuelve descriptor o -1 si falla.
    if (fdw < 0) {
        perror("open FIFO_REQ for write");              // No se pudo abrir en escritura.
        return -1;
    }

    // Escribe la Request completa (protocolo binario de tamaño fijo: sizeof(*req)).
    ssize_t w = write(fdw, req, sizeof(*req));          // write retorna bytes escritos o -1.
    if (w != (ssize_t)sizeof(*req)) {                   // Verifica que se envió todo el struct.
        if (w < 0) perror("write FIFO_REQ");            // Si falló write, perrorea la causa.
        close(fdw);                                     // Cierra descriptor de escritura.
        return -1;
    }
    close(fdw);                                         // Cierra FIFO_REQ (lado escritor).

    // Abre FIFO_RES sólo para LECTURA; se bloqueará hasta que el daemon lo abra en ESCRITURA.
    int fdr = open(FIFO_RES, O_RDONLY);                 // Devuelve descriptor o -1 si falla.
    if (fdr < 0) {
        perror("open FIFO_RES for read");               // No se pudo abrir en lectura.
        return -1;
    }

    // Lee la Response completa (tamaño fijo: sizeof(*res)).
    ssize_t r = read(fdr, res, sizeof(*res));           // read retorna bytes leídos o -1.
    close(fdr);                                         // Cierra FIFO_RES (lado lector).
    if (r != (ssize_t)sizeof(*res)) {                   // Verifica que se recibió toda la Response.
        if (r < 0) perror("read FIFO_RES");             // Si falló read, perrorea la causa.
        return -1;
    }

    return 0;                                           // Éxito total de ida y vuelta.
}

/* -------------------------------------------------------------------------- */
/* main: loop de menú y orquestación de la UI */
int main(void) {
    // Buffers que mantienen los criterios elegidos por el usuario entre iteraciones del menú:
    char title_buf[256] = {0};    // Criterio 1: título. Tamaño alineado con common.h.
    char date_buf[256]  = {0};    // Criterio 2: fecha. Tamaño alineado con common.h.

    while (1) {                                        // Bucle principal hasta que se elija “Salir”.
        print_menu(title_buf, date_buf);               // Dibuja menú con valores actuales.

        char opt_line[64];                             // Buffer para leer la opción elegida.
        if (!fgets(opt_line, sizeof(opt_line), stdin)) {// Lee una línea desde stdin; NULL=EOF/Error.
            printf("\nSaliendo...\n");                 // Si EOF (Ctrl-D) o error, salimos amable.
            break;                                     // Rompe el bucle principal.
        }
        trim_inplace(opt_line);                        // Limpia espacios y saltos finales.
        if (opt_line[0] == '\0') continue;             // Línea vacía: reimprime menú.

        int opt = atoi(opt_line);                      // Convierte a entero (basta para 1..4).

        if (opt == 1) {                                // Opción 1: Capturar “title”.
            printf("Ingrese primer criterio de búsqueda (title): ");
            fflush(stdout);                            // Fuerza a imprimir el prompt ya.
            char input[MAX_INPUT];                     // Buffer grande para el título.
            if (!fgets(input, sizeof(input), stdin)) { // Lee la línea completa (o hasta MAX_INPUT-1).
                printf("\nEntrada interrumpida. Volviendo al menú.\n");
                continue;                              // Regresa al menú en caso de EOF/error.
            }
            input[strcspn(input, "\r\n")] = '\0';      // Elimina \r/\n finales si los hay.
            trim_inplace(input);                       // Quita espacios laterales.
            strncpy(title_buf, input, sizeof(title_buf)-1); // Copia segura (deja 1 byte para '\0').
            title_buf[sizeof(title_buf)-1] = '\0';     // Garantiza terminación.
            continue;                                  // Vuelve al menú (muestra title actualizado).

        } else if (opt == 2) {                         // Opción 2: Capturar/validar “date”.
            printf("Ingrese segundo criterio de búsqueda (date): ");
            fflush(stdout);                            // Imprime prompt.
            char input[MAX_INPUT];                     // Buffer grande para la fecha.
            if (!fgets(input, sizeof(input), stdin)) { // Lee línea de fecha.
                printf("\nEntrada interrumpida. Volviendo al menú.\n");
                continue;                              // Regresa al menú.
            }
            input[strcspn(input, "\r\n")] = '\0';      // Elimina \r/\n finales.
            trim_inplace(input);                       // Quita espacios laterales.

            if (input[0] != '\0') {                    // Si no está vacía, validar formato y rangos.
                if (!valid_ymd_format(input)) {        // YYYY-MM-DD + bisiesto.
                    printf("Formato de fecha inválido. Use YYYY-MM-DD. Volviendo al menú.\n");
                    continue;                          // No guarda; reimprime menú.
                }
            }
            strncpy(date_buf, input, sizeof(date_buf)-1); // Copia fecha válida (o cadena vacía).
            date_buf[sizeof(date_buf)-1] = '\0';       // Asegura terminación.
            continue;                                  // Vuelve al menú (muestra date actualizado).

        } else if (opt == 3) {                         // Opción 3: Armar Request, medir, enviar, mostrar.
            Request req;                               // Estructura a enviar al daemon.
            Response res;                              // Estructura a recibir desde daemon.
            memset(&req, 0, sizeof(req));              // Limpia la Request completa a cero.
            memset(&res, 0, sizeof(res));              // Limpia la Response completa a cero.

            // Campo 1 (title): sólo si el usuario ingresó algo.
            if (title_buf[0] != '\0') {
                snprintf(req.field_name1, sizeof(req.field_name1), "%s", "title");       // Nombre del campo.
                snprintf(req.value1, sizeof(req.value1), "%s", title_buf);               // Valor buscado (literal).
            } else {
                req.field_name1[0] = '\0';            // Campos vacíos si no aplica.
                req.value1[0] = '\0';
            }

            // Campo 2 (date): si el usuario ingresó fecha válida.
            if (date_buf[0] != '\0') {
                snprintf(req.field_name2, sizeof(req.field_name2), "%s", "update_date"); // Nombre real del dataset.
                snprintf(req.value2, sizeof(req.value2), "%s", date_buf);                // Valor buscado.
            } else {
                req.field_name2[0] = '\0';            // Campos vacíos si no aplica.
                req.value2[0] = '\0';
            }

            printf("Realizando búsqueda...\n");        // Feedback al usuario.
            fflush(stdout);                            // Asegura que el texto salga ya.

            struct timespec t1, t2;                    // Marcas de tiempo para latencia.
            clock_gettime(CLOCK_MONOTONIC, &t1);       // Toma tiempo “antes” (monótonico no salta).

            int ok = send_request_and_get_response(&req, &res); // Hace IPC: FIFO_REQ -> FIFO_RES.

            clock_gettime(CLOCK_MONOTONIC, &t2);       // Toma tiempo “después”.
            double elapsed =                            // Calcula delta en segundos con precisión ns.
                (t2.tv_sec - t1.tv_sec) +
                (t2.tv_nsec - t1.tv_nsec) / 1e9;

            if (ok != 0) {                             // Si IPC falló (daemon no está, FIFO roto, etc.).
                printf("Error comunicándose con el buscador. Asegúrate de que el daemon está corriendo.\n");
                continue;                              // Vuelve al menú.
            }

            printf(">> Tiempo que tardó la búsqueda: %.3f segundos\n", elapsed); // Muestra latencia.
            printf(">> Resultado de la búsqueda:\n");  // Encabezado de resultados.

            // Nota: asumimos que Response tiene un campo 'result' (char[]). La UI imprime “tal cual”.
            if (res.result[0] == '\0') {               // Si daemon envió vacío...
                printf("\n");                          // Muestra línea en blanco.
                // Si el requisito pide "NA", el daemon debería cargar "NA\n" en res.result.
            } else {
                size_t len = strnlen(res.result, sizeof(res.result)); // Longitud acotada al buffer.
                if (len > 0) {
                    fwrite(res.result, 1, len, stdout); // Imprime binario “seguro” (sin asumir %s).
                    if (res.result[len-1] != '\n') putchar('\n'); // Garantiza salto de línea final.
                } else {
                    putchar('\n');                    // Caso corner: len==0 pero no '\0' inicial.
                }
            }

            continue;                                  // Tras mostrar, vuelve al menú.

        } else if (opt == 4) {                         // Opción 4: Salir.
            printf("Saliendo...\n");                   // Mensaje de despedida.
            break;                                     // Corta el while(1).

        } else {                                       // Opción fuera de 1..4.
            printf("Opción no válida. Intenta de nuevo.\n");
            continue;                                  // Repite menú.
        }
    }

    return 0;                                          // Fin normal del programa.
}
