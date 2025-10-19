# === Makefile para la Práctica 1 ===
# Compila:
#  - p1-dataProgram  (UI)
#  - p1-search       (daemon / worker)

CC = gcc
CFLAGS = -std=c11 -O2 -Wall -Wextra -D_GNU_SOURCE
TARGET_UI = p1-dataProgram
TARGET_WORKER = p1-search

# Archivos fuente
SRC_UI = p1-dataProgram.c
SRC_WORKER = p1-search.c index2.c hash.c

# Archivos de cabecera
HEADERS = common.h index.h hash.h

# === Regla por defecto ===
all: $(TARGET_UI) $(TARGET_WORKER)

# === Compilar la UI ===
$(TARGET_UI): $(SRC_UI) $(HEADERS)
	$(CC) $(CFLAGS) -o $(TARGET_UI) $(SRC_UI)

# === Compilar el worker ===
$(TARGET_WORKER): $(SRC_WORKER) $(HEADERS)
	$(CC) $(CFLAGS) -o $(TARGET_WORKER) $(SRC_WORKER)

# === Limpieza ===
clean:
	rm -f $(TARGET_UI) $(TARGET_WORKER) *.o

# === Recompilar desde cero ===
rebuild: clean all

.PHONY: all clean rebuild

