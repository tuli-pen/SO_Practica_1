# Práctica 1 Sistemas Operativos

## Integrantes del Grupo:
- Misael Jesús Florez Anave
- Nicolay Prieto Mendoza
- Tuli Peña Melo

# Compilación:
make

# Ejecución:
Terminal 1:
./p1-search

Terminal 2:
./p1-dataProgram "arxiv.csv"
  
# Dataset elegido: 
"arxiv"
- Dataset de 1.7 millones de papers de investigación.
- Pesa 3.9GB.
  
# Campos del dataset y tipo de datos de cada uno:
  id — numérico, float

  submitter — string

  authors — string

  title — string ← campo indexado (primario)

  abstract — string

  categories — string

  comments — string
  
  journal-ref — string
  
  doi — string
  
  report-no — string
  
  license — string

  update_date — fecha en formato YYYY-MM-DD ← campo secundario (filtro opcional)

  versions_count — entero

  versions_last_created — fecha y hora
  
# Criterios de búsqueda implementados:
- title: se busca la cadena ingresada dentro de title, case-insensitive. Heurística de subcadena: Para poder ofrecer búsqueda por subcadenas sin reconstruir el índice, el search worker calcula el bucket del hash de la cadena buscada y escanea los buckets vecinos (un rango simétrico de ±12 por defecto).

- update_date: filtro por igualdad exacta en formato YYYY-MM-DD.

Justificación: el objetivo es permitir al usuario buscar por partes del título — por ejemplo, palabras clave o fragmentos — y obtener coincidencias relevantes. El índice está construido sobre títulos completos; para subcadenas usamos una heurística. El filtro por fecha permite acotar resultados por fecha de actualización cuando el usuario lo requiera.
  
# Rangos de valores válidos para cada campo de entrada
- title: texto. Longitud de 1 a 255 caracteres.
- update_date: texto. Verificación de formato YYYY-MM-DD.

# Descripción de las estructuras de datos utilizadas
Tabla Hash — BucketDisk[]
- Cada BucketDisk contiene first_entry_offset (offset al primer EntryDisk del bucket o -1 si vacío).
- Representa la tabla de N_BUCKETS posiciones. Se guarda inmediatamente después del header.

Lista enlazada en disco — EntryDisk
- Campos: char key[KEY_SIZE] (título, fixed-size), long csv_offset (byte offset del registro en arxiv.csv), long next_entry (offset al siguiente EntryDisk o -1).
- Uso: cada entrada del índice apunta al offset en el CSV para leer la línea completa cuando hay match. Las colisiones se resuelven encadenando EntryDisk en una lista ligada sobre disco.

# Ejemplos específicos de uso

## Ejemplo 1 — búsqueda por subcadena 
1. Opción 1 (Ingresar primer criterio de búsqueda) 
2. Escribe: dark matter
3. Opción 3 (Realizar búsqueda)
4. Resultado esperado:

Realizando búsqueda...
>> Tiempo que tardó la búsqueda: 0.038 segundos
>> Resultado de la búsqueda:
"astro-ph/0508263","Srdjan Samurovic","S. Samurovic and I.J. Danziger","Dark matter in early-type galaxies: dynamical modelling of IC1459, IC3370, NGC3379 and NGC4105","We analyse long-slit spectra of four early-type galaxies 
...


## Ejemplo 2 — búsqueda por título + filtro de fecha
1. Opción 1 (Ingresar primer criterio de búsqueda) 
2. Escribe: A determinant of Stirling cycle numbers counts unlabeled acyclic single-source automata
3. Opción 2 (Ingresar segundo criterio de búsqueda)
4. Escribe: 2007-05-23
5. Opción 3 (Realizar búsqueda)
6. Resultado esperado:

Realizando búsqueda...
>> Tiempo que tardó la búsqueda: 1.795 segundos
>> Resultado de la búsqueda:
"0704.0004","David Callan","David Callan","A determinant of Stirling cycle numbers counts unlabeled acyclic single-source automata","We show that a determinant of Stirling cycle numbers counts unlabeled acyclic single-source automata. 
...
