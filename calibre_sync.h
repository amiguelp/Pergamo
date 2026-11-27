#ifndef PERGAMO_CALIBRE_SYNC_H
#define PERGAMO_CALIBRE_SYNC_H

#include <stddef.h>
#include "opds_app.h"

#define CALIBRE_REPORT_TITLES 5

typedef struct {
    int finished;            /* libros terminados en el PocketBook */
    int previously_synced;   /* ya resueltos en sincronizaciones anteriores */
    int marked;              /* marcados ahora como leídos */
    int already;             /* ya estaban leídos en Calibre-Web */
    int not_found;           /* no encontrados en Calibre-Web */
    int ambiguous;           /* varios candidatos: no se tocan */
    int errors;              /* no se pudo comprobar o marcar */
    int unmatched_titles_count;
    char unmatched_titles[CALIBRE_REPORT_TITLES][96];
} CalibreSyncReport;

typedef void (*CalibreSyncProgress)(int done, int total, const char *title);

/* ¿Parece un servidor Calibre-Web con usuario y contraseña? */
int CalibreLooksLikeServer(const OPDSServer *server);

/* Apunta qué libro de Calibre-Web es un archivo descargado con Pergamo. */
void CalibreRecordDownload(const char *server_url, const char *download_url,
                           const char *local_path);

/* Marca en Calibre-Web los libros terminados en el lector. retry_unmatched:
   volver a buscar los que antes no se encontraron. max_books: como mucho
   cuántos libros procesar ahora (0 = todos). 0 si terminó (aunque haya
   libros sin encontrar), -1 si no se pudo (sin acceso, etc.). */
int CalibreSyncRead(const OPDSServer *server, int retry_unmatched, int max_books,
                    CalibreSyncProgress progress, CalibreSyncReport *report,
                    char *error, size_t error_size);

#endif
