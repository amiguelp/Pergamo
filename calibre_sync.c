/*
 * Sincronizar con Calibre-Web los libros terminados en el PocketBook.
 *
 * 1. Lee (solo lectura) la base de datos de la biblioteca del lector,
 *    /mnt/ext1/system/explorer-3/explorer-3.db: books_settings.completed = 1.
 *    libsqlite3 se carga con dlopen para no ser una dependencia fija de la app.
 * 2. Identifica cada libro en Calibre-Web:
 *    - por el número de libro apuntado al descargarlo con Pergamo
 *      (calibre_links.cfg), o
 *    - buscándolo por OPDS (título exacto y autor coincidente; si hay varios
 *      candidatos, no se toca).
 * 3. Inicia sesión en la web (una vez por sincronización: Calibre-Web limita
 *    los inicios de sesión) y, para cada libro, mira la casilla "leído" de su
 *    ficha (/book/<id>). Solo si no está marcada llama a
 *    /ajax/toggleread/<id>, que alterna el estado, y lo comprueba después.
 *    Nunca desmarca nada.
 * El resultado de cada libro se guarda en read_sync.cfg para no repetirlo.
 */
#include "calibre_sync.h"
#include "opds_app.h"

#include <curl/curl.h>
#include <ctype.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

extern void LogDebug(const char *msg);

#ifndef PB_LIBRARY_DB
#define PB_LIBRARY_DB "/mnt/ext1/system/explorer-3/explorer-3.db"
#endif
#ifndef CALIBRE_LINKS_FILE
#define CALIBRE_LINKS_FILE APP_ROOT_DIR "calibre_links.cfg"
#endif
#ifndef READ_SYNC_FILE
#define READ_SYNC_FILE APP_ROOT_DIR "read_sync.cfg"
#endif

#define MAX_FINISHED 2000
#define MAX_PATHS 4

/* ------------------------------------------------------------------------ */
/* Utilidades                                                               */
/* ------------------------------------------------------------------------ */

typedef struct {
    char *data;
    size_t size;
} Buffer;

static size_t BufferWrite(void *contents, size_t size, size_t nmemb, void *userp) {
    Buffer *b = userp;
    size_t n = size * nmemb;
    if (b->size + n > 8 * 1024 * 1024) return 0;
    char *grown = realloc(b->data, b->size + n + 1);
    if (!grown) return 0;
    b->data = grown;
    memcpy(b->data + b->size, contents, n);
    b->size += n;
    b->data[b->size] = '\0';
    return n;
}

static void BufferFree(Buffer *b) {
    free(b->data);
    b->data = NULL;
    b->size = 0;
}

/* Minúsculas, sin tildes (Latin-1 y algunas más) y solo letras/números
   separados por un espacio. Sirve para comparar títulos y autores. */
static void Normalize(const char *in, char *out, size_t out_size) {
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

/* Algún apellido/nombre (3+ letras) del autor del lector aparece en el de
   Calibre-Web. */
static int AuthorsMatch(const char *pb_author, const char *cw_authors) {
    char a[512], b[1024];
    Normalize(pb_author, a, sizeof(a));
    Normalize(cw_authors, b, sizeof(b));
    if (!a[0] || !b[0]) return !a[0];   /* sin autor en el lector: solo título */
    char padded[1100];
    snprintf(padded, sizeof(padded), " %s ", b);
    char *save = NULL;
    for (char *tok = strtok_r(a, " ", &save); tok; tok = strtok_r(NULL, " ", &save)) {
        if (strlen(tok) < 3) continue;
        char needle[64];
        snprintf(needle, sizeof(needle), " %s ", tok);
        if (strstr(padded, needle)) return 1;
    }
    return 0;
}

static void XmlUnescape(char *s) {
    static const struct { const char *from; char to; } map[] = {
        {"&amp;", '&'}, {"&lt;", '<'}, {"&gt;", '>'}, {"&quot;", '"'}, {"&apos;", '\''}, {"&#39;", '\''}
    };
    char *w = s;
    for (char *r = s; *r;) {
        int done = 0;
        if (*r == '&') {
            for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
                size_t n = strlen(map[i].from);
                if (strncmp(r, map[i].from, n) == 0) { *w++ = map[i].to; r += n; done = 1; break; }
            }
        }
        if (!done) *w++ = *r++;
    }
    *w = '\0';
}

static int ExtractBetween(const char *start, const char *end_limit, const char *open,
                          const char *close, char *out, size_t out_size) {
    const char *a = strstr(start, open);
    if (!a || (end_limit && a >= end_limit)) return 0;
    a += strlen(open);
    const char *b = strstr(a, close);
    if (!b || (end_limit && b > end_limit)) return 0;
    size_t n = (size_t)(b - a);
    if (n >= out_size) n = out_size - 1;
    memcpy(out, a, n);
    out[n] = '\0';
    XmlUnescape(out);
    return 1;
}

/* Número de libro de Calibre-Web en un enlace .../opds/download/<n>/... */
static long BookIdFromURL(const char *url) {
    const char *keys[] = {"/opds/download/", "/opds/cover/", "/book/"};
    for (size_t i = 0; url && i < sizeof(keys) / sizeof(keys[0]); i++) {
        const char *p = strstr(url, keys[i]);
        if (!p) continue;
        p += strlen(keys[i]);
        if (!isdigit((unsigned char)*p)) continue;
        return strtol(p, NULL, 10);
    }
    return 0;
}

static void BaseURL(const char *server_url, char *out, size_t out_size) {
    snprintf(out, out_size, "%s", server_url);
    char *opds = strstr(out, "/opds");
    if (opds) *opds = '\0';
    size_t n = strlen(out);
    while (n > 0 && out[n - 1] == '/') out[--n] = '\0';
}

int CalibreLooksLikeServer(const OPDSServer *server) {
    return server && strstr(server->url, "/opds") && server->user[0] &&
           (server->pass[0] || server->pass_locked) &&
           !strstr(server->url, "ebiblio.es");
}

/* ------------------------------------------------------------------------ */
/* Enlaces guardados al descargar                                           */
/* ------------------------------------------------------------------------ */

void CalibreRecordDownload(const char *server_url, const char *download_url,
                           const char *local_path) {
    long id = BookIdFromURL(download_url);
    if (id <= 0 || !local_path || !local_path[0] || !strstr(download_url, "/opds/download/")) return;
    FILE *f = fopen(CALIBRE_LINKS_FILE, "a");
    if (!f) return;
    fprintf(f, "%s\t%s\t%ld\n", local_path, server_url, id);
    fclose(f);
}

static long LinkedBookId(const char *server_url, const char *path) {
    FILE *f = fopen(CALIBRE_LINKS_FILE, "r");
    if (!f) return 0;
    char line[1600];
    long found = 0;
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = '\0';
        char *tab1 = strchr(line, '\t');
        char *tab2 = tab1 ? strchr(tab1 + 1, '\t') : NULL;
        if (!tab2) continue;
        *tab1 = *tab2 = '\0';
        if (strcmp(line, path) == 0 && strcmp(tab1 + 1, server_url) == 0) {
            found = strtol(tab2 + 1, NULL, 10);   /* el último apunte manda */
        }
    }
    fclose(f);
    return found;
}

/* ------------------------------------------------------------------------ */
/* Estado de sincronización                                                 */
/* ------------------------------------------------------------------------ */

/* Devuelve el último estado guardado ("marked", "already", "notfound",
   "ambiguous"...) o "" si el libro no se ha procesado con este servidor. */
static void SyncState(const char *server_url, long long pb_id, char *state, size_t n) {
    state[0] = '\0';
    FILE *f = fopen(READ_SYNC_FILE, "r");
    if (!f) return;
    char line[1200];
    char key[64];
    snprintf(key, sizeof(key), "%lld", pb_id);
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = '\0';
        char *f1 = strchr(line, '\t');
        char *f2 = f1 ? strchr(f1 + 1, '\t') : NULL;
        char *f3 = f2 ? strchr(f2 + 1, '\t') : NULL;
        if (!f2) continue;
        *f1 = *f2 = '\0';
        if (f3) *f3 = '\0';
        if (strcmp(line, server_url) == 0 && strcmp(f1 + 1, key) == 0) snprintf(state, n, "%s", f2 + 1);
    }
    fclose(f);
}

static void SaveSyncState(const char *server_url, long long pb_id, const char *state, long cw_id) {
    FILE *f = fopen(READ_SYNC_FILE, "a");
    if (!f) return;
    fprintf(f, "%s\t%lld\t%s\t%ld\t%ld\n", server_url, pb_id, state, cw_id, (long)time(NULL));
    fclose(f);
}

/* ------------------------------------------------------------------------ */
/* Biblioteca del PocketBook (SQLite cargado en tiempo de ejecución)        */
/* ------------------------------------------------------------------------ */

typedef struct sqlite3 sqlite3;
typedef struct sqlite3_stmt sqlite3_stmt;
#define SQ_OPEN_READONLY 0x00000001
#define SQ_ROW 100

static struct {
    void *handle;
    int (*open_v2)(const char *, sqlite3 **, int, const char *);
    int (*prepare_v2)(sqlite3 *, const char *, int, sqlite3_stmt **, const char **);
    int (*step)(sqlite3_stmt *);
    const unsigned char *(*column_text)(sqlite3_stmt *, int);
    long long (*column_int64)(sqlite3_stmt *, int);
    int (*finalize)(sqlite3_stmt *);
    int (*close)(sqlite3 *);
    int (*busy_timeout)(sqlite3 *, int);
} sq;

static int LoadSqlite(void) {
    if (sq.handle) return 0;
    const char *names[] = {"libsqlite3.so.0", "libsqlite3.so"};
    for (size_t i = 0; i < 2 && !sq.handle; i++) sq.handle = dlopen(names[i], RTLD_NOW);
    if (!sq.handle) return -1;
    *(void **)&sq.open_v2 = dlsym(sq.handle, "sqlite3_open_v2");
    *(void **)&sq.prepare_v2 = dlsym(sq.handle, "sqlite3_prepare_v2");
    *(void **)&sq.step = dlsym(sq.handle, "sqlite3_step");
    *(void **)&sq.column_text = dlsym(sq.handle, "sqlite3_column_text");
    *(void **)&sq.column_int64 = dlsym(sq.handle, "sqlite3_column_int64");
    *(void **)&sq.finalize = dlsym(sq.handle, "sqlite3_finalize");
    *(void **)&sq.close = dlsym(sq.handle, "sqlite3_close");
    *(void **)&sq.busy_timeout = dlsym(sq.handle, "sqlite3_busy_timeout");
    if (!sq.open_v2 || !sq.prepare_v2 || !sq.step || !sq.column_text || !sq.column_int64 ||
        !sq.finalize || !sq.close) {
        dlclose(sq.handle);
        memset(&sq, 0, sizeof(sq));
        return -1;
    }
    return 0;
}

typedef struct {
    long long id;
    char title[256];
    char author[256];
    char paths[MAX_PATHS][512];
    int path_count;
} FinishedBook;

static const char *ColumnText(sqlite3_stmt *st, int i) {
    const unsigned char *t = sq.column_text(st, i);
    return t ? (const char *)t : "";
}

static int ReadFinishedBooks(FinishedBook **books, int *count, char *error, size_t error_size) {
    *books = NULL;
    *count = 0;
    if (LoadSqlite() != 0) {
        snprintf(error, error_size, "%s", T("No se pudo leer la biblioteca del PocketBook (falta SQLite)."));
        return -1;
    }
    sqlite3 *db = NULL;
    if (sq.open_v2(PB_LIBRARY_DB, &db, SQ_OPEN_READONLY, NULL) != 0 || !db) {
        if (db) sq.close(db);
        snprintf(error, error_size, "%s", T("No se pudo abrir la biblioteca del PocketBook."));
        return -1;
    }
    if (sq.busy_timeout) sq.busy_timeout(db, 3000);
    const char *query =
        "SELECT b.id, IFNULL(b.title,''), IFNULL(b.author,''), IFNULL(fo.name,''), IFNULL(f.filename,'') "
        "FROM books_settings s JOIN books_impl b ON b.id = s.bookid "
        "LEFT JOIN files f ON f.book_id = b.id LEFT JOIN folders fo ON fo.id = f.folder_id "
        "WHERE s.completed = 1 ORDER BY b.id";
    sqlite3_stmt *st = NULL;
    if (sq.prepare_v2(db, query, -1, &st, NULL) != 0 || !st) {
        sq.close(db);
        snprintf(error, error_size, "%s", T("La biblioteca del PocketBook tiene un formato desconocido."));
        return -1;
    }
    FinishedBook *list = calloc(MAX_FINISHED, sizeof(FinishedBook));
    if (!list) {
        sq.finalize(st);
        sq.close(db);
        snprintf(error, error_size, "%s", T("No hay memoria suficiente."));
        return -1;
    }
    int n = 0;
    while (sq.step(st) == SQ_ROW) {
        long long id = sq.column_int64(st, 0);
        FinishedBook *b = (n > 0 && list[n - 1].id == id) ? &list[n - 1] : NULL;
        if (!b) {
            if (n >= MAX_FINISHED) break;
            b = &list[n++];
            b->id = id;
            snprintf(b->title, sizeof(b->title), "%s", ColumnText(st, 1));
            snprintf(b->author, sizeof(b->author), "%s", ColumnText(st, 2));
        }
        const char *folder = ColumnText(st, 3);
        const char *file = ColumnText(st, 4);
        if (file[0] && b->path_count < MAX_PATHS) {
            if (file[0] == '/' || !folder[0])
                snprintf(b->paths[b->path_count++], sizeof(b->paths[0]), "%s", file);
            else
                snprintf(b->paths[b->path_count++], sizeof(b->paths[0]), "%s/%s", folder, file);
        }
    }
    sq.finalize(st);
    sq.close(db);
    *books = list;
    *count = n;
    return 0;
}

/* ------------------------------------------------------------------------ */
/* Calibre-Web                                                              */
/* ------------------------------------------------------------------------ */

typedef struct {
    CURL *curl;
    char base[MAX_STR_LEN];
    char userpwd[MAX_STR_LEN * 2 + 2];
    char csrf[256];
} Session;

static void ExtractCsrf(const char *html, char *out, size_t out_size) {
    out[0] = '\0';
    const char *p = html ? strstr(html, "name=\"csrf_token\"") : NULL;
    if (!p) return;
    const char *tag_start = p;
    while (tag_start > html && *tag_start != '<') tag_start--;
    const char *tag_end = strchr(p, '>');
    const char *v = strstr(tag_start, "value=\"");
    if (!v || (tag_end && v > tag_end)) return;
    v += 7;
    const char *e = strchr(v, '"');
    if (!e) return;
    size_t n = (size_t)(e - v);
    if (n >= out_size) n = out_size - 1;
    memcpy(out, v, n);
    out[n] = '\0';
}

/* method: "GET" o "POST". basic_auth: enviar usuario y contraseña (OPDS). */
static int Request(Session *s, const char *url, const char *method, const char *body,
                   int basic_auth, Buffer *out, long *status, char *redirect, size_t redirect_size) {
    CURL *c = s->curl;
    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, BufferWrite);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, out);
    if (basic_auth) {
        curl_easy_setopt(c, CURLOPT_HTTPAUTH, (long)CURLAUTH_BASIC);
        curl_easy_setopt(c, CURLOPT_USERPWD, s->userpwd);
    } else {
        curl_easy_setopt(c, CURLOPT_USERPWD, NULL);
    }
    struct curl_slist *headers = NULL;
    char csrf_header[300];
    if (strcmp(method, "POST") == 0) {
        curl_easy_setopt(c, CURLOPT_POST, 1L);
        curl_easy_setopt(c, CURLOPT_POSTFIELDS, body ? body : "");
        if (s->csrf[0]) {
            snprintf(csrf_header, sizeof(csrf_header), "X-CSRFToken: %s", s->csrf);
            headers = curl_slist_append(headers, csrf_header);
        }
    } else {
        curl_easy_setopt(c, CURLOPT_HTTPGET, 1L);
    }
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, headers);
    CURLcode code = curl_easy_perform(c);
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, NULL);
    curl_slist_free_all(headers);
    *status = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, status);
    if (redirect && redirect_size) {
        char *loc = NULL;
        redirect[0] = '\0';
        if (curl_easy_getinfo(c, CURLINFO_REDIRECT_URL, &loc) == CURLE_OK && loc)
            snprintf(redirect, redirect_size, "%s", loc);
    }
    if (code != CURLE_OK) {
        char line[256];
        snprintf(line, sizeof(line), "Calibre-Web %s %s -> curl %d", method, url, (int)code);
        LogDebug(line);
        return -1;
    }
    return 0;
}

static int SessionOpen(Session *s, const OPDSServer *server, char *error, size_t error_size) {
    memset(s, 0, sizeof(*s));
    BaseURL(server->url, s->base, sizeof(s->base));
    snprintf(s->userpwd, sizeof(s->userpwd), "%s:%s", server->user, server->pass);
    s->curl = curl_easy_init();
    if (!s->curl) {
        snprintf(error, error_size, "%s", T("No se pudo iniciar la conexión HTTP."));
        return -1;
    }
    curl_easy_setopt(s->curl, CURLOPT_COOKIEFILE, "");      /* cookies en memoria */
    curl_easy_setopt(s->curl, CURLOPT_USERAGENT, APP_USER_AGENT);
    curl_easy_setopt(s->curl, CURLOPT_CONNECTTIMEOUT, 20L);
    curl_easy_setopt(s->curl, CURLOPT_LOW_SPEED_TIME, 30L);
    curl_easy_setopt(s->curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
    /* Igual que el resto de conexiones OPDS de Pergamo (servidores caseros
       con certificados propios). */
    curl_easy_setopt(s->curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(s->curl, CURLOPT_SSL_VERIFYHOST, 0L);
    return 0;
}

static void SessionClose(Session *s) {
    if (s->curl) curl_easy_cleanup(s->curl);
    s->curl = NULL;
    memset(s->userpwd, 0, sizeof(s->userpwd));
}

static int Login(Session *s, const OPDSServer *server, char *error, size_t error_size) {
    char url[MAX_STR_LEN + 32];
    Buffer page = {0};
    long status = 0;
    snprintf(url, sizeof(url), "%s/login", s->base);
    if (Request(s, url, "GET", NULL, 0, &page, &status, NULL, 0) != 0 || status != 200) {
        BufferFree(&page);
        snprintf(error, error_size, T("No se pudo abrir la página de acceso de Calibre-Web (HTTP %ld)."), status);
        return -1;
    }
    ExtractCsrf(page.data, s->csrf, sizeof(s->csrf));
    int looks_like_calibre_web = page.data && strstr(page.data, "name=\"username\"") &&
                                 strstr(page.data, "name=\"password\"");
    BufferFree(&page);
    if (!looks_like_calibre_web) {
        snprintf(error, error_size, "%s", T("Este servidor no parece Calibre-Web."));
        return -1;
    }

    char *user = curl_easy_escape(s->curl, server->user, 0);
    char *pass = curl_easy_escape(s->curl, server->pass, 0);
    char *token = curl_easy_escape(s->curl, s->csrf, 0);
    size_t body_size = strlen(user) + strlen(pass) + strlen(token) + 96;
    char *body = malloc(body_size);
    if (body) snprintf(body, body_size, "username=%s&password=%s&remember_me=on&csrf_token=%s&next=%%2F",
                       user, pass, token);
    curl_free(user);
    curl_free(token);
    if (pass) { memset(pass, 0, strlen(pass)); curl_free(pass); }
    if (!body) {
        snprintf(error, error_size, "%s", T("No hay memoria suficiente."));
        return -1;
    }
    Buffer reply = {0};
    char location[MAX_STR_LEN] = {0};
    int r = Request(s, url, "POST", body, 0, &reply, &status, location, sizeof(location));
    memset(body, 0, body_size);
    free(body);
    BufferFree(&reply);
    if (r != 0) {
        snprintf(error, error_size, "%s", T("No se pudo conectar con Calibre-Web."));
        return -1;
    }
    if (status == 429) {
        snprintf(error, error_size, "%s", T("Calibre-Web ha bloqueado temporalmente el acceso por demasiados intentos. Inténtalo dentro de un rato."));
        return -1;
    }
    if (status < 300 || status >= 400 || strstr(location, "/login")) {
        snprintf(error, error_size, "%s", T("Calibre-Web rechazó el usuario o la contraseña."));
        return -1;
    }
    return 0;
}

/* 1 = leído, 0 = no leído, -1 = no se pudo saber. Actualiza el token CSRF. */
static int ReadStatus(Session *s, long id) {
    char url[MAX_STR_LEN + 48];
    snprintf(url, sizeof(url), "%s/book/%ld", s->base, id);
    Buffer page = {0};
    long status = 0;
    if (Request(s, url, "GET", NULL, 0, &page, &status, NULL, 0) != 0 || status != 200 || !page.data) {
        BufferFree(&page);
        return -1;
    }
    char token[256];
    ExtractCsrf(page.data, token, sizeof(token));
    if (token[0]) snprintf(s->csrf, sizeof(s->csrf), "%s", token);
    int result = -1;
    const char *p = strstr(page.data, "id=\"have_read_cb\"");
    if (p) {
        const char *start = p;
        while (start > page.data && *start != '<') start--;
        const char *end = strchr(p, '>');
        if (end) {
            size_t n = (size_t)(end - start);
            char tag[512];
            if (n >= sizeof(tag)) n = sizeof(tag) - 1;
            memcpy(tag, start, n);
            tag[n] = '\0';
            /* Solo el atributo "checked" en sí: Calibre-Web también pone
               data-checked="..." y data-unchecked="..." en esta casilla. */
            result = 0;
            for (const char *c = strstr(tag, "checked"); c; c = strstr(c + 7, "checked")) {
                char before = c > tag ? c[-1] : ' ';
                char after = c[7];
                if ((before == ' ' || before == '\t' || before == '\n' || before == '\r') &&
                    (after == '\0' || after == ' ' || after == '=' || after == '/' ||
                     after == '>' || after == '\t' || after == '\n' || after == '\r')) {
                    result = 1;
                    break;
                }
            }
        }
    }
    BufferFree(&page);
    return result;
}

static int ToggleRead(Session *s, long id) {
    char url[MAX_STR_LEN + 48];
    snprintf(url, sizeof(url), "%s/ajax/toggleread/%ld", s->base, id);
    char *token = curl_easy_escape(s->curl, s->csrf, 0);
    char body[400];
    snprintf(body, sizeof(body), "csrf_token=%s", token ? token : "");
    curl_free(token);
    Buffer reply = {0};
    long status = 0;
    int r = Request(s, url, "POST", body, 0, &reply, &status, NULL, 0);
    BufferFree(&reply);
    return (r == 0 && status == 200) ? 0 : -1;
}

/* Busca el libro por OPDS. Devuelve el número, 0 si no se encuentra o -1
   si hay más de un candidato. */
static long FindBook(Session *s, const FinishedBook *book) {
    if (!book->title[0]) return 0;
    char *q = curl_easy_escape(s->curl, book->title, 0);
    char url[MAX_STR_LEN * 3];
    snprintf(url, sizeof(url), "%s/opds/search?query=%s", s->base, q ? q : "");
    curl_free(q);
    Buffer feed = {0};
    long status = 0;
    if (Request(s, url, "GET", NULL, 1, &feed, &status, NULL, 0) != 0 || status != 200 || !feed.data) {
        BufferFree(&feed);
        return 0;
    }
    char want[512];
    Normalize(book->title, want, sizeof(want));
    long found = 0;
    int ambiguous = 0;
    for (char *entry = strstr(feed.data, "<entry"); entry; entry = strstr(entry + 6, "<entry")) {
        char *end = strstr(entry, "</entry>");
        if (!end) break;
        char title[512] = {0}, authors[1024] = {0}, name[256];
        ExtractBetween(entry, end, "<title>", "</title>", title, sizeof(title));
        for (char *a = strstr(entry, "<author>"); a && a < end; a = strstr(a + 8, "<author>")) {
            if (ExtractBetween(a, end, "<name>", "</name>", name, sizeof(name))) {
                strncat(authors, name, sizeof(authors) - strlen(authors) - 2);
                strncat(authors, " ", sizeof(authors) - strlen(authors) - 1);
            }
        }
        long id = 0;
        for (char *h = strstr(entry, "href=\""); h && h < end && !id; h = strstr(h + 6, "href=\"")) {
            char href[512];
            if (ExtractBetween(h, end, "href=\"", "\"", href, sizeof(href))) id = BookIdFromURL(href);
        }
        char got[512];
        Normalize(title, got, sizeof(got));
        if (id > 0 && strcmp(got, want) == 0 && AuthorsMatch(book->author, authors)) {
            if (found == 0) found = id;
            else if (found != id) ambiguous = 1;
        }
    }
    BufferFree(&feed);
    return ambiguous ? -1 : found;
}

/* ------------------------------------------------------------------------ */

int CalibreSyncRead(const OPDSServer *server, int retry_unmatched, int max_books,
                    CalibreSyncProgress progress, CalibreSyncReport *report,
                    char *error, size_t error_size) {
    memset(report, 0, sizeof(*report));
    if (error && error_size) error[0] = '\0';

    FinishedBook *books = NULL;
    int count = 0;
    if (ReadFinishedBooks(&books, &count, error, error_size) != 0) return -1;
    report->finished = count;

    /* Solo los que aún no se han resuelto con este servidor */
    int *pending = calloc(count > 0 ? count : 1, sizeof(int));
    int pending_count = 0;
    for (int i = 0; pending && i < count; i++) {
        char state[32];
        SyncState(server->url, books[i].id, state, sizeof(state));
        int unresolved = !state[0] || strcmp(state, "error") == 0 ||
                         (retry_unmatched && (strcmp(state, "notfound") == 0 || strcmp(state, "ambiguous") == 0 ||
                                              strcmp(state, "unknown") == 0));
        if (unresolved) pending[pending_count++] = i;
        else if (strcmp(state, "marked") == 0 || strcmp(state, "already") == 0) report->previously_synced++;
    }
    if (max_books > 0 && pending_count > max_books) pending_count = max_books;
    if (pending_count == 0) {
        free(pending);
        free(books);
        return 0;
    }

    Session s;
    if (SessionOpen(&s, server, error, error_size) != 0 || Login(&s, server, error, error_size) != 0) {
        SessionClose(&s);
        free(pending);
        free(books);
        return -1;
    }

    for (int k = 0; k < pending_count; k++) {
        FinishedBook *b = &books[pending[k]];
        if (progress) progress(k, pending_count, b->title);
        long id = 0;
        for (int p = 0; p < b->path_count && !id; p++) id = LinkedBookId(server->url, b->paths[p]);
        if (!id) id = FindBook(&s, b);
        if (id == 0 || id == -1) {
            SaveSyncState(server->url, b->id, id == 0 ? "notfound" : "ambiguous", 0);
            if (id == 0) report->not_found++; else report->ambiguous++;
            int listed = 0;
            for (int t = 0; t < report->unmatched_titles_count; t++)
                if (strncmp(report->unmatched_titles[t], b->title, 90) == 0) listed = 1;
            if (!listed && report->unmatched_titles_count < CALIBRE_REPORT_TITLES) {
                snprintf(report->unmatched_titles[report->unmatched_titles_count++],
                         sizeof(report->unmatched_titles[0]), "%.90s", b->title);
            }
            continue;
        }
        int read = ReadStatus(&s, id);
        if (read == 1) {
            SaveSyncState(server->url, b->id, "already", id);
            report->already++;
        } else if (read == 0) {
            if (ToggleRead(&s, id) == 0 && ReadStatus(&s, id) == 1) {
                SaveSyncState(server->url, b->id, "marked", id);
                report->marked++;
                char line[400];
                snprintf(line, sizeof(line), "Calibre-Web: marcado como leído #%ld %s", id, b->title);
                LogDebug(line);
            } else {
                SaveSyncState(server->url, b->id, "error", id);
                report->errors++;
            }
        } else {
            SaveSyncState(server->url, b->id, "unknown", id);
            report->errors++;
        }
    }
    if (progress) progress(pending_count, pending_count, "");
    SessionClose(&s);
    free(pending);
    free(books);
    return 0;
}
