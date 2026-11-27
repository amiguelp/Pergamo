#include "pblibrary.h"

#include <ctype.h>
#include <dlfcn.h>
#include <string.h>

#define SQ_OPEN_READONLY 0x00000001

static PbSqlite sq;
static void *sq_handle = NULL;

const PbSqlite *PbSqliteLoad(void) {
    if (sq_handle) return &sq;
    const char *names[] = {"libsqlite3.so.0", "libsqlite3.so"};
    for (size_t i = 0; i < 2 && !sq_handle; i++) sq_handle = dlopen(names[i], RTLD_NOW);
    if (!sq_handle) return NULL;
    *(void **)&sq.open_v2 = dlsym(sq_handle, "sqlite3_open_v2");
    *(void **)&sq.prepare_v2 = dlsym(sq_handle, "sqlite3_prepare_v2");
    *(void **)&sq.step = dlsym(sq_handle, "sqlite3_step");
    *(void **)&sq.column_text = dlsym(sq_handle, "sqlite3_column_text");
    *(void **)&sq.column_int64 = dlsym(sq_handle, "sqlite3_column_int64");
    *(void **)&sq.finalize = dlsym(sq_handle, "sqlite3_finalize");
    *(void **)&sq.close = dlsym(sq_handle, "sqlite3_close");
    *(void **)&sq.busy_timeout = dlsym(sq_handle, "sqlite3_busy_timeout");
    if (!sq.open_v2 || !sq.prepare_v2 || !sq.step || !sq.column_text || !sq.column_int64 ||
        !sq.finalize || !sq.close) {
        dlclose(sq_handle);
        sq_handle = NULL;
        memset(&sq, 0, sizeof(sq));
        return NULL;
    }
    return &sq;
}

sqlite3 *PbLibraryOpen(void) {
    const PbSqlite *s = PbSqliteLoad();
    if (!s) return NULL;
    sqlite3 *db = NULL;
    if (s->open_v2(PB_LIBRARY_DB, &db, SQ_OPEN_READONLY, NULL) != 0 || !db) {
        if (db) s->close(db);
        return NULL;
    }
    if (s->busy_timeout) s->busy_timeout(db, 3000);
    return db;
}

void PbLibraryClose(sqlite3 *db) {
    if (db && sq_handle) sq.close(db);
}

const char *PbColumnText(sqlite3_stmt *st, int column) {
    const unsigned char *t = sq_handle ? sq.column_text(st, column) : NULL;
    return t ? (const char *)t : "";
}

/* Minúsculas, sin tildes (Latin-1 y algunas más) y solo letras/números
   separados por un espacio. Sirve para comparar títulos y autores. */
void PbNormalize(const char *in, char *out, size_t out_size) {
    size_t o = 0;
    int space = 1;
    const unsigned char *p = (const unsigned char *)in;
    while (p && *p && o + 2 < out_size) {
        unsigned char c = *p;
        char mapped = 0;
        if (c < 0x80) {
            mapped = isalnum(c) ? (char)tolower(c) : ' ';
            p++;
        } else if (c == 0xC3 && p[1]) {
            static const char latin1[] =
                /* 0x80-0xBF de U+00C0..U+00FF */
                "aaaaaaaceeeeiiii" "dnooooo ouuuuyts"
                "aaaaaaaceeeeiiii" "dnooooo ouuuuyty";
            unsigned char d = p[1];
            mapped = (d >= 0x80 && d <= 0xBF) ? latin1[d - 0x80] : ' ';
            p += 2;
        } else {
            /* otros caracteres multibyte: se tratan como separador */
            p++;
            while ((*p & 0xC0) == 0x80) p++;
            mapped = ' ';
        }
        if (mapped == ' ') {
            if (!space) { out[o++] = ' '; space = 1; }
        } else {
            out[o++] = mapped;
            space = 0;
        }
    }
    while (o > 0 && out[o - 1] == ' ') o--;
    out[o] = '\0';
}

