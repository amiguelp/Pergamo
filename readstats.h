#ifndef PERGAMO_READSTATS_H
#define PERGAMO_READSTATS_H

#include <stddef.h>

#define STATS_MAX_YEARS 30
#define STATS_MAX_ONGOING 5

typedef struct {
    int books_finished;
    int books_ongoing;
    int finished_with_pages;
    long pages_total;
    long pages_finished;
    long pages_ongoing;
    int avg_pages;
    char longest_title[96];
    int longest_pages;
    struct { int year; int books; long pages; } years[STATS_MAX_YEARS];
    int year_count;
    /* months[0] = mes actual, months[1] = el anterior... (year = 0 si vacío) */
    struct { int year; int month; int books; long pages; } months[12];
    struct { char title[96]; int percent; } ongoing[STATS_MAX_ONGOING];
    int ongoing_count;
    /* de dónde salen las páginas de los libros con lectura */
    int source_exact, source_estimated, source_online, source_reader, source_none;
    int pending_real_pages;   /* libros cuyas páginas reales aún no se han buscado */
} ReadingStats;

typedef struct {
    int processed, exact, estimated, online, not_found, skipped;
} StatsUpdateReport;

typedef void (*StatsProgress)(int done, int total, const char *title);

/* Rápido: usa las páginas ya guardadas. */
int StatsCompute(ReadingStats *stats, char *error, size_t error_size);

/* Busca las páginas reales de los libros que aún no las tienen (abre los
   EPUB y, si no hay archivo, consulta Open Library). */
int StatsUpdatePages(StatsProgress progress, StatsUpdateReport *report,
                     char *error, size_t error_size);

#endif
