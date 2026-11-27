/*
 * Estadísticas de lectura a partir de la biblioteca del PocketBook.
 *
 * Páginas "reales" de cada libro (se guardan en pages_cache.cfg):
 *   P  exactas: marcas de página de la edición impresa en el EPUB, o las
 *      páginas de un PDF/DjVu (fijas)
 *   E  estimadas: caracteres del texto del EPUB / EPUB_CHARS_PER_PAGE
 *   O  Open Library: páginas de la edición impresa (por ISBN o título y autor)
 *   N  no encontradas en Open Library (se reintenta pasados 30 días)
 * Si un libro no tiene páginas reales se usan las del lector (L), salvo que
 * sean 100 (el lector cuenta entonces en porcentaje).
 */
#include "readstats.h"
#include "opds_app.h"
#include "pblibrary.h"
#include "epubpages.h"

#include <curl/curl.h>
#include <json-c/json.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

extern void LogDebug(const char *msg);

#ifndef PAGES_CACHE_FILE
#define PAGES_CACHE_FILE APP_ROOT_DIR "pages_cache.cfg"
#endif
#define MAX_STAT_BOOKS 1500
#define RETRY_NOT_FOUND (30L * 24 * 3600)

typedef struct {
    char key[600];
    char title[256];
    char author[256];
    char isbn[20];
    int finished;
    long finished_ts;      /* la primera vez que se terminó */
    long open_ts;          /* la última vez que se abrió */
    double fraction;
    int reader_pages;
    int pages;
    char source;
    char paths[4][512];
    int path_count;
} StatBook;

/* --- caché de páginas ---------------------------------------------------- */

typedef struct { char key[600]; int pages; char source; long ts; } CacheEntry;
static CacheEntry *cache = NULL;
static int cache_count = 0, cache_cap = 0;

static void CacheLoad(void) {
    cache_count = 0;
    FILE *f = fopen(PAGES_CACHE_FILE, "r");
    if (!f) return;
    char line[800];
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = '\0';
        char *t1 = strchr(line, '\t'), *t2 = t1 ? strchr(t1 + 1, '\t') : NULL, *t3 = t2 ? strchr(t2 + 1, '\t') : NULL;
        if (!t3) continue;
        *t1 = *t2 = *t3 = '\0';
        if (cache_count == cache_cap) {
            int cap = cache_cap ? cache_cap * 2 : 256;
            CacheEntry *grown = realloc(cache, sizeof(CacheEntry) * cap);
            if (!grown) break;
            cache = grown;
            cache_cap = cap;
        }
        CacheEntry *e = &cache[cache_count++];
        snprintf(e->key, sizeof(e->key), "%s", line);
        e->pages = atoi(t1 + 1);
        e->source = t2[1];
        e->ts = atol(t3 + 1);
    }
    fclose(f);
}

static const CacheEntry *CacheFind(const char *key) {
    for (int i = cache_count - 1; i >= 0; i--)        /* el último apunte manda */
        if (strcmp(cache[i].key, key) == 0) return &cache[i];
    return NULL;
}

static void CacheAppend(const char *key, int pages, char source) {
    FILE *f = fopen(PAGES_CACHE_FILE, "a");
    if (!f) return;
    fprintf(f, "%s\t%d\t%c\t%ld\n", key, pages, source, (long)time(NULL));
    fclose(f);
}

/* --- biblioteca ---------------------------------------------------------- */

static int HasExtension(const char *path, const char *ext) {
    size_t n = strlen(path), e = strlen(ext);
    return n > e && strcasecmp(path + n - e, ext) == 0;
}

static int LoadBooks(StatBook **out, int *count, char *error, size_t error_size) {
    *out = NULL;
    *count = 0;
    const PbSqlite *sq = PbSqliteLoad();
    sqlite3 *db = sq ? PbLibraryOpen() : NULL;
    if (!db) {
        snprintf(error, error_size, "%s", T("No se pudo abrir la biblioteca del PocketBook."));
        return -1;
    }
    const char *query =
        "SELECT b.id, IFNULL(b.title,''), IFNULL(b.author,''), IFNULL(b.isbn,''), "
        "IFNULL(s.cpage,0), IFNULL(s.npage,0), IFNULL(s.completed,0), IFNULL(s.completed_ts,0), "
        "IFNULL(fo.name,''), IFNULL(f.filename,''), IFNULL(s.opentime,0) "
        "FROM books_settings s JOIN books_impl b ON b.id = s.bookid "
        "LEFT JOIN files f ON f.book_id = b.id LEFT JOIN folders fo ON fo.id = f.folder_id "
        "ORDER BY b.id";
    sqlite3_stmt *st = NULL;
    if (sq->prepare_v2(db, query, -1, &st, NULL) != 0 || !st) {
        PbLibraryClose(db);
        snprintf(error, error_size, "%s", T("La biblioteca del PocketBook tiene un formato desconocido."));
        return -1;
    }
    StatBook *books = calloc(MAX_STAT_BOOKS, sizeof(StatBook));
    if (!books) {
        sq->finalize(st);
        PbLibraryClose(db);
        snprintf(error, error_size, "%s", T("No hay memoria suficiente."));
        return -1;
    }
    int n = 0;
    long long last_id = -1;
    while (sq->step(st) == PB_SQLITE_ROW) {
        long long id = sq->column_int64(st, 0);
        const char *title = PbColumnText(st, 1), *author = PbColumnText(st, 2);
        char key[600], nt[256], na[256];
        PbNormalize(title, nt, sizeof(nt));
        PbNormalize(author, na, sizeof(na));
        snprintf(key, sizeof(key), "%s|%s", nt, na);
        if (!nt[0]) continue;
        /* Duplicados del mismo libro (copia descargada y de la nube, etc.):
           se juntan en uno */
        StatBook *b = NULL;
        for (int i = 0; i < n && !b; i++) if (strcmp(books[i].key, key) == 0) b = &books[i];
        int cpage = (int)sq->column_int64(st, 4), npage = (int)sq->column_int64(st, 5);
        int completed = (int)sq->column_int64(st, 6);
        long completed_ts = (long)sq->column_int64(st, 7);
        if (!b) {
            if (n >= MAX_STAT_BOOKS) break;
            b = &books[n++];
            snprintf(b->key, sizeof(b->key), "%s", key);
            snprintf(b->title, sizeof(b->title), "%s", title);
            snprintf(b->author, sizeof(b->author), "%s", author);
        }
        if (id != last_id) {
            last_id = id;
            const char *isbn = PbColumnText(st, 3);
            if (!b->isbn[0] && isbn[0]) snprintf(b->isbn, sizeof(b->isbn), "%s", isbn);
            double fraction = npage > 0 ? (double)cpage / npage : (completed ? 1.0 : 0.0);
            if (fraction > 1.0) fraction = 1.0;
            int finished = completed == 1 || (npage > 1 && cpage >= npage);
            if (finished) fraction = 1.0;
            if (fraction > b->fraction) b->fraction = fraction;
            if (finished) {
                b->finished = 1;
                /* Las copias de la nube se marcan al sincronizarse: vale la
                   primera fecha en que se terminó */
                if (completed_ts > 0 && (b->finished_ts == 0 || completed_ts < b->finished_ts))
                    b->finished_ts = completed_ts;
            }
            long open_ts = (long)sq->column_int64(st, 10);
            if (open_ts > b->open_ts) b->open_ts = open_ts;
            if (npage > b->reader_pages) b->reader_pages = npage;
        }
        const char *folder = PbColumnText(st, 8), *file = PbColumnText(st, 9);
        if (file[0] && b->path_count < 4) {
            if (file[0] == '/' || !folder[0])
                snprintf(b->paths[b->path_count++], sizeof(b->paths[0]), "%s", file);
            else
                snprintf(b->paths[b->path_count++], sizeof(b->paths[0]), "%s/%s", folder, file);
        }
    }
    sq->finalize(st);
    PbLibraryClose(db);
    *out = books;
    *count = n;
    return 0;
}

/* Páginas para las estadísticas: las reales (caché) o las del lector. */
static void ResolvePages(StatBook *b) {
    const CacheEntry *e = CacheFind(b->key);
    if (e && e->pages > 0 && e->source != 'N') {
        b->pages = e->pages;
        b->source = e->source;
    } else if (b->reader_pages > 0 && b->reader_pages != 100) {
        b->pages = b->reader_pages;
        b->source = 'L';
    } else {
        b->pages = 0;
        b->source = 0;
    }
}

static int NeedsRealPages(const StatBook *b) {
    if (b->fraction <= 0.0) return 0;
    const CacheEntry *e = CacheFind(b->key);
    if (!e) return 1;
    return e->source == 'N' && time(NULL) - e->ts > RETRY_NOT_FOUND;
}

/* --- estadísticas ----------------------------------------------------------- */

int StatsCompute(ReadingStats *s, char *error, size_t error_size) {
    memset(s, 0, sizeof(*s));
    StatBook *books;
    int n;
    if (LoadBooks(&books, &n, error, error_size) != 0) return -1;
    CacheLoad();
    time_t now = time(NULL);
    struct tm today;
    localtime_r(&now, &today);
    int this_month = today.tm_year * 12 + today.tm_mon;
    for (int i = 0; i < n; i++) {
        StatBook *b = &books[i];
        if (b->fraction <= 0.0) continue;
        ResolvePages(b);
        if (NeedsRealPages(b)) s->pending_real_pages++;
        switch (b->source) {
            case 'P': s->source_exact++; break;
            case 'E': s->source_estimated++; break;
            case 'O': s->source_online++; break;
            case 'L': s->source_reader++; break;
            default: s->source_none++; break;
        }
        if (b->finished) {
            s->books_finished++;
            s->pages_finished += b->pages;
            if (b->pages > 0) s->finished_with_pages++;
            if (b->pages > s->longest_pages) {
                s->longest_pages = b->pages;
                snprintf(s->longest_title, sizeof(s->longest_title), "%.90s", b->title);
            }
            if (b->finished_ts > 0) {
                time_t t = (time_t)b->finished_ts;
                struct tm tm;
                localtime_r(&t, &tm);
                int year = tm.tm_year + 1900;
                int y;
                for (y = 0; y < s->year_count && s->years[y].year != year; y++) {}
                if (y == s->year_count && s->year_count < STATS_MAX_YEARS) {
                    s->years[s->year_count].year = year;
                    s->year_count++;
                }
                if (y < s->year_count) { s->years[y].books++; s->years[y].pages += b->pages; }
                int age = this_month - (tm.tm_year * 12 + tm.tm_mon);
                if (age >= 0 && age < 12) {
                    s->months[age].year = year;
                    s->months[age].month = tm.tm_mon + 1;
                    s->months[age].books++;
                    s->months[age].pages += b->pages;
                }
            }
        } else {
            s->books_ongoing++;
            s->pages_ongoing += (long)(b->pages * b->fraction + 0.5);
        }
    }
    /* "Leyendo ahora": los abiertos más recientemente (al menos un 1 %) */
    for (int k = 0; k < STATS_MAX_ONGOING; k++) {
        int best = -1;
        for (int i = 0; i < n; i++) {
            StatBook *b = &books[i];
            if (b->finished || b->fraction < 0.01 || b->open_ts < 0) continue;
            if (best < 0 || b->open_ts > books[best].open_ts) best = i;
        }
        if (best < 0) break;
        snprintf(s->ongoing[s->ongoing_count].title, sizeof(s->ongoing[0].title), "%.80s", books[best].title);
        s->ongoing[s->ongoing_count].percent = (int)(books[best].fraction * 100 + 0.5);
        s->ongoing_count++;
        books[best].open_ts = -1;            /* ya incluido */
    }
    s->pages_total = s->pages_finished + s->pages_ongoing;
    s->avg_pages = s->finished_with_pages ? (int)(s->pages_finished / s->finished_with_pages) : 0;
    /* años de más reciente a más antiguo */
    for (int a = 0; a < s->year_count; a++)
        for (int b = a + 1; b < s->year_count; b++)
            if (s->years[b].year > s->years[a].year) {
                __typeof__(s->years[0]) t = s->years[a]; s->years[a] = s->years[b]; s->years[b] = t;
            }
    free(books);
    return 0;
}

/* --- Open Library ---------------------------------------------------------- */

typedef struct { char *data; size_t size; } Mem;

static size_t MemWrite(void *p, size_t size, size_t nmemb, void *userp) {
    Mem *m = userp;
    size_t n = size * nmemb;
    if (m->size + n > 2 * 1024 * 1024) return 0;
    char *grown = realloc(m->data, m->size + n + 1);
    if (!grown) return 0;
    m->data = grown;
    memcpy(m->data + m->size, p, n);
    m->size += n;
    m->data[m->size] = '\0';
    return n;
}

/* >0 páginas, 0 no encontrado, -1 error de red. */
static int OpenLibraryPages(const StatBook *b) {
    CURL *curl = curl_easy_init();
    if (!curl) return -1;
    char url[1200];
    if (b->isbn[0]) {
        snprintf(url, sizeof(url),
                 "https://openlibrary.org/search.json?isbn=%s&fields=title,number_of_pages_median&limit=5", b->isbn);
    } else {
        char *t = curl_easy_escape(curl, b->title, 0);
        char *a = curl_easy_escape(curl, b->author, 0);
        snprintf(url, sizeof(url),
                 "https://openlibrary.org/search.json?title=%s%s%s&fields=title,number_of_pages_median&limit=5",
                 t ? t : "", b->author[0] ? "&author=" : "", b->author[0] && a ? a : "");
        curl_free(t);
        curl_free(a);
    }
    Mem m = {NULL, 0};
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, MemWrite);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &m);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "Pergamo/" PERGAMO_VERSION " (PocketBook; https://github.com/amiguelp/Pergamo)");
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 20L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 60L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    CURLcode code = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_cleanup(curl);
    if (code != CURLE_OK || status != 200 || !m.data) { free(m.data); return -1; }

    int pages = 0;
    char want[256];
    PbNormalize(b->title, want, sizeof(want));
    struct json_object *root = json_tokener_parse(m.data), *docs = NULL;
    free(m.data);
    if (root && json_object_object_get_ex(root, "docs", &docs) && json_object_is_type(docs, json_type_array)) {
        for (size_t i = 0; i < json_object_array_length(docs) && !pages; i++) {
            struct json_object *doc = json_object_array_get_idx(docs, i), *title = NULL, *count = NULL;
            if (!json_object_object_get_ex(doc, "number_of_pages_median", &count)) continue;
            int value = json_object_get_int(count);
            if (value <= 0) continue;
            if (b->isbn[0]) { pages = value; break; }                /* por ISBN: vale */
            if (!json_object_object_get_ex(doc, "title", &title)) continue;
            char got[256];
            PbNormalize(json_object_get_string(title), got, sizeof(got));
            if (strcmp(got, want) == 0) pages = value;               /* por título: exacto */
        }
    }
    if (root) json_object_put(root);
    return pages;
}

/* --- actualizar páginas reales -------------------------------------------- */

int StatsUpdatePages(StatsProgress progress, StatsUpdateReport *report, char *error, size_t error_size) {
    memset(report, 0, sizeof(*report));
    StatBook *books;
    int n;
    if (LoadBooks(&books, &n, error, error_size) != 0) return -1;
    CacheLoad();
    int *todo = malloc(sizeof(int) * (n > 0 ? n : 1));
    int todo_count = 0;
    for (int i = 0; todo && i < n; i++) if (NeedsRealPages(&books[i])) todo[todo_count++] = i;
    int network_errors = 0;
    for (int k = 0; k < todo_count; k++) {
        StatBook *b = &books[todo[k]];
        if (progress) progress(k, todo_count, b->title);
        const char *file = NULL;
        for (int p = 0; p < b->path_count && !file; p++) if (access(b->paths[p], R_OK) == 0) file = b->paths[p];

        if (file && (HasExtension(file, ".pdf") || HasExtension(file, ".djvu")) && b->reader_pages > 0) {
            CacheAppend(b->key, b->reader_pages, 'P');            /* páginas fijas */
            report->exact++;
            continue;
        }
        if (file && HasExtension(file, ".epub")) {
            EpubPageInfo info;
            if (EpubAnalyze(file, &info) == 0) {
                if (!b->isbn[0] && info.isbn[0]) snprintf(b->isbn, sizeof(b->isbn), "%s", info.isbn);
                if (info.print_pages > 0) {
                    CacheAppend(b->key, info.print_pages, 'P');
                    report->exact++;
                    continue;
                }
                if (info.estimated_pages >= 5) {
                    CacheAppend(b->key, info.estimated_pages, 'E');
                    report->estimated++;
                    continue;
                }
            }
        }
        /* Sin archivo (préstamos devueltos, libros borrados...) o EPUB ilegible */
        if (network_errors >= 3) { report->skipped++; continue; }
        int pages = OpenLibraryPages(b);
        if (pages > 0) {
            CacheAppend(b->key, pages, 'O');
            report->online++;
        } else if (pages == 0) {
            CacheAppend(b->key, 0, 'N');
            report->not_found++;
        } else {
            network_errors++;
            report->skipped++;
        }
        usleep(300000);     /* no saturar Open Library */
    }
    if (progress) progress(todo_count, todo_count, "");
    report->processed = todo_count;
    free(todo);
    free(books);
    return 0;
}
