/*
 * Actualización automática de Pergamo desde el repositorio público:
 *   https://github.com/amiguelp/Pergamo  (rama main)
 *
 * En main se publican, juntos en el mismo commit:
 *   version.txt      -> versión publicada, p. ej. "1.2"
 *   Pergamo.app      -> la aplicación
 *   Pergamo.app.b2   -> BLAKE2b-256 (hex) de Pergamo.app
 * (los genera tools/make_release_files.py; "make release").
 *
 * Instalación: se descarga a "<app>.new", se comprueba la huella y que sea
 * un ejecutable ARM, se guarda la versión actual como "<app>.old" y se
 * sustituye. Si algo falla se deja la versión instalada como estaba.
 */
#include "updater.h"
#include "opds_app.h"
#include "launcher_icon.h"
#include "monocypher.h"

#include <curl/curl.h>
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

extern void LogDebug(const char *msg);

#ifndef UPDATE_BASE_URL
#define UPDATE_BASE_URL "https://raw.githubusercontent.com/amiguelp/Pergamo/main/"
#endif
#define MAX_TEXT 4096
#define MAX_APP_SIZE (30L * 1024 * 1024)
#define MIN_APP_SIZE (64L * 1024)

typedef struct {
    char data[MAX_TEXT];
    size_t size;
    int overflow;
} TextBuffer;

static size_t WriteText(void *contents, size_t size, size_t nmemb, void *userp) {
    TextBuffer *buffer = userp;
    size_t n = size * nmemb;
    if (buffer->size + n >= sizeof(buffer->data)) {
        buffer->overflow = 1;
        return 0;
    }
    memcpy(buffer->data + buffer->size, contents, n);
    buffer->size += n;
    buffer->data[buffer->size] = '\0';
    return n;
}

static void SetCurlError(CURLcode code, long status, char *error, size_t error_size) {
    if (!error || !error_size) return;
    if (code == CURLE_OK) {
        snprintf(error, error_size, T("GitHub respondió con HTTP %ld."), status);
    } else if (code == CURLE_COULDNT_RESOLVE_HOST || code == CURLE_COULDNT_CONNECT) {
        snprintf(error, error_size, "%s", T("No se pudo conectar con GitHub. Comprueba la conexión Wi-Fi."));
    } else if (code == CURLE_OPERATION_TIMEDOUT) {
        snprintf(error, error_size, "%s", T("La conexión con GitHub agotó el tiempo de espera."));
    } else {
        snprintf(error, error_size, T("Error de red: %s"), curl_easy_strerror(code));
    }
}

static CURL *NewRequest(const char *url) {
    CURL *curl = curl_easy_init();
    if (!curl) return NULL;
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS, (long)CURLPROTO_HTTPS);
    curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS, (long)CURLPROTO_HTTPS);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 20L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 30L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, APP_USER_AGENT);
    return curl;
}

/* GET de un archivo de texto pequeño de main (sin caché intermedia). */
static int FetchText(const char *name, TextBuffer *buffer, char *error, size_t error_size) {
    char url[512];
    snprintf(url, sizeof(url), UPDATE_BASE_URL "%s?nc=%ld", name, (long)time(NULL));
    memset(buffer, 0, sizeof(*buffer));
    CURL *curl = NewRequest(url);
    if (!curl) {
        snprintf(error, error_size, "%s", T("No se pudo iniciar la conexión HTTP."));
        return -1;
    }
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteText);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, buffer);
    CURLcode code = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_cleanup(curl);
    if (code != CURLE_OK || status != 200 || buffer->overflow) {
        SetCurlError(buffer->overflow ? CURLE_OK : code, status, error, error_size);
        return -1;
    }
    /* recortar espacios y saltos de línea */
    while (buffer->size && isspace((unsigned char)buffer->data[buffer->size - 1])) {
        buffer->data[--buffer->size] = '\0';
    }
    return 0;
}

int CompareVersions(const char *a, const char *b) {
    while ((a && *a) || (b && *b)) {
        long x = 0, y = 0;
        if (a && *a) { x = strtol(a, (char **)&a, 10); if (*a == '.') a++; }
        if (b && *b) { y = strtol(b, (char **)&b, 10); if (*b == '.') b++; }
        if (x != y) return x < y ? -1 : 1;
    }
    return 0;
}

static int ValidVersion(const char *v) {
    if (!v[0] || strlen(v) > 15 || !isdigit((unsigned char)v[0])) return 0;
    for (const char *p = v; *p; p++) {
        if (!isdigit((unsigned char)*p) && *p != '.') return 0;
    }
    return 1;
}

int UpdateCheck(char *remote_version, size_t remote_size, char *error, size_t error_size) {
    if (error && error_size) error[0] = '\0';
    TextBuffer buffer;
    if (FetchText("version.txt", &buffer, error, error_size) != 0) return -1;
    if (!ValidVersion(buffer.data)) {
        snprintf(error, error_size, "%s", T("La versión publicada no es válida."));
        return -1;
    }
    snprintf(remote_version, remote_size, "%s", buffer.data);
    char log_line[128];
    snprintf(log_line, sizeof(log_line), "Update check: installed %s, published %s",
             PERGAMO_VERSION, remote_version);
    LogDebug(log_line);
    return CompareVersions(remote_version, PERGAMO_VERSION) > 0 ? 1 : 0;
}

/* --- Instalación ------------------------------------------------------- */

static void AppPath(char *out, size_t out_size) {
    ssize_t n = readlink("/proc/self/exe", out, out_size - 1);
    if (n > 0 && (size_t)n < out_size - 1) {
        out[n] = '\0';
        if (strstr(out, ".app")) return;
    }
    snprintf(out, out_size, "%s", OPDS_APP_BINARY_PATH);
}

typedef struct {
    UpdateProgressFn progress;
    int last_percent;
} ProgressState;

static int OnProgress(void *userp, curl_off_t total, curl_off_t now,
                      curl_off_t ul_total, curl_off_t ul_now) {
    (void)ul_total; (void)ul_now;
    ProgressState *state = userp;
    if (total > MAX_APP_SIZE || now > MAX_APP_SIZE) return 1;   /* aborta */
    if (state->progress && total > 0) {
        int percent = (int)(now * 100 / total);
        if (percent >= state->last_percent + 5 || percent == 100) {
            state->last_percent = percent;
            state->progress(percent);
        }
    }
    return 0;
}

static int HashFile(const char *path, uint8_t hash[32], long *size) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    crypto_blake2b_ctx ctx;
    crypto_blake2b_init(&ctx, 32);
    uint8_t buffer[16384];
    size_t n;
    *size = 0;
    while ((n = fread(buffer, 1, sizeof(buffer), f)) > 0) {
        crypto_blake2b_update(&ctx, buffer, n);
        *size += (long)n;
    }
    int ok = !ferror(f);
    fclose(f);
    crypto_blake2b_final(&ctx, hash);
    return ok ? 0 : -1;
}

static int IsArmExecutable(const char *path) {
    unsigned char header[20];
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    size_t n = fread(header, 1, sizeof(header), f);
    fclose(f);
    /* ELF, 32 bits, little-endian, e_machine = 40 (ARM) */
    return n == sizeof(header) && memcmp(header, "\x7f" "ELF", 4) == 0 &&
           header[4] == 1 && header[5] == 1 && header[18] == 40 && header[19] == 0;
}

int UpdateInstall(UpdateProgressFn progress, char *error, size_t error_size) {
    if (error && error_size) error[0] = '\0';

    TextBuffer expected;
    if (FetchText("Pergamo.app.b2", &expected, error, error_size) != 0) return -1;
    if (expected.size < 64) {
        snprintf(error, error_size, "%s", T("La huella publicada no es válida."));
        return -1;
    }

    char app_path[512], new_path[530], old_path[530];
    AppPath(app_path, sizeof(app_path));
    snprintf(new_path, sizeof(new_path), "%s.new", app_path);
    snprintf(old_path, sizeof(old_path), "%s.old", app_path);

    FILE *out = fopen(new_path, "wb");
    if (!out) {
        snprintf(error, error_size, "%s", T("No se pudo crear el archivo de descarga."));
        return -1;
    }
    char url[512];
    snprintf(url, sizeof(url), UPDATE_BASE_URL "Pergamo.app?nc=%ld", (long)time(NULL));
    CURL *curl = NewRequest(url);
    if (!curl) {
        fclose(out);
        unlink(new_path);
        snprintf(error, error_size, "%s", T("No se pudo iniciar la conexión HTTP."));
        return -1;
    }
    ProgressState state = { progress, -100 };
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, NULL);   /* fwrite por defecto */
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, out);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, OnProgress);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &state);
    CURLcode code = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_cleanup(curl);
    int write_error = fclose(out) != 0;
    if (code != CURLE_OK || status != 200 || write_error) {
        unlink(new_path);
        if (write_error) snprintf(error, error_size, "%s", T("No se pudo guardar la descarga (¿memoria llena?)."));
        else SetCurlError(code, status, error, error_size);
        return -1;
    }

    /* Comprobaciones antes de tocar la app instalada */
    uint8_t hash[32];
    long size = 0;
    char hex[65];
    if (HashFile(new_path, hash, &size) != 0) {
        unlink(new_path);
        snprintf(error, error_size, "%s", T("No se pudo leer la descarga."));
        return -1;
    }
    for (int i = 0; i < 32; i++) snprintf(hex + i * 2, 3, "%02x", hash[i]);
    if (strncasecmp(hex, expected.data, 64) != 0 || size < MIN_APP_SIZE || !IsArmExecutable(new_path)) {
        unlink(new_path);
        LogDebug("Update rejected: checksum or file type mismatch.");
        snprintf(error, error_size, "%s",
                 T("La descarga no coincide con la versión publicada. No se ha cambiado nada; inténtalo más tarde."));
        return -1;
    }

    /* Sustituir: la versión actual queda como .old por si hay que volver atrás */
    unlink(old_path);
    if (rename(app_path, old_path) != 0) {
        unlink(new_path);
        snprintf(error, error_size, "%s", T("No se pudo sustituir la aplicación."));
        return -1;
    }
    if (rename(new_path, app_path) != 0) {
        rename(old_path, app_path);
        unlink(new_path);
        snprintf(error, error_size, "%s", T("No se pudo sustituir la aplicación."));
        return -1;
    }
    sync();
    LogDebug("Update installed.");
    return 0;
}

void UpdateCleanupPreviousVersion(void) {
    char app_path[512], old_path[530], new_path[530];
    AppPath(app_path, sizeof(app_path));
    snprintf(old_path, sizeof(old_path), "%s.old", app_path);
    snprintf(new_path, sizeof(new_path), "%s.new", app_path);
    if (unlink(old_path) == 0) LogDebug("Removed previous version after a successful update.");
    unlink(new_path);
}
