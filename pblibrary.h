#ifndef PERGAMO_PBLIBRARY_H
#define PERGAMO_PBLIBRARY_H

/* Acceso de solo lectura a la base de datos de la biblioteca del PocketBook
   (/mnt/ext1/system/explorer-3/explorer-3.db). libsqlite3 se carga con
   dlopen para no ser una dependencia fija de la app. */

#ifndef PB_LIBRARY_DB
#define PB_LIBRARY_DB "/mnt/ext1/system/explorer-3/explorer-3.db"
#endif

typedef struct sqlite3 sqlite3;
typedef struct sqlite3_stmt sqlite3_stmt;

typedef struct {
    int (*open_v2)(const char *, sqlite3 **, int, const char *);
    int (*prepare_v2)(sqlite3 *, const char *, int, sqlite3_stmt **, const char **);
    int (*step)(sqlite3_stmt *);
    const unsigned char *(*column_text)(sqlite3_stmt *, int);
    long long (*column_int64)(sqlite3_stmt *, int);
    int (*finalize)(sqlite3_stmt *);
    int (*close)(sqlite3 *);
    int (*busy_timeout)(sqlite3 *, int);
} PbSqlite;

#define PB_SQLITE_ROW 100

/* NULL si no se puede cargar libsqlite3. */
const PbSqlite *PbSqliteLoad(void);

/* Abre la biblioteca en solo lectura. NULL si no se puede. */
sqlite3 *PbLibraryOpen(void);
void PbLibraryClose(sqlite3 *db);

/* Texto de una columna ("" si es NULL). */
const char *PbColumnText(sqlite3_stmt *st, int column);

#include <stddef.h>

/* Minúsculas, sin tildes y solo letras/números separados por un espacio
   (para comparar títulos y autores). */
void PbNormalize(const char *in, char *out, size_t out_size);

#endif
