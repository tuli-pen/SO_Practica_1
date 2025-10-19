#ifndef COMMON_H
#define COMMON_H
#define FIFO_REQ "/tmp/p1_req"
#define FIFO_RES "/tmp/p1_res"

// Mensaje que la UI envia al daemon
typedef struct {
    char field_name1[64];
    char value1[256];
    char field_name2[64];  // vacio si no se usa
    char value2[256];
} Request;

// Respuesta que el daemon devuelve a la UI
typedef struct {
    // Si no hay resultados, el daemon debe enviar "NA"
    char result[2048];
} Response;

#endif
