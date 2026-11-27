#include "opds_app.h"
#include <curl/curl.h>
#include <stdlib.h>
#include <string.h>

extern void LogDebug(const char *msg);
extern void ShowDownloadProgress(long long total, long long current);
extern int CheckDownloadCancel();
extern char current_host[MAX_STR_LEN]; // Populated by ui.c

// RELATIVE URL SAFETY NET
void EnsureAbsoluteURL(const char *in_url, char *out_url) {
    if (strncmp(in_url, "http://", 7) == 0 || strncmp(in_url, "https://", 8) == 0) {
        strncpy(out_url, in_url, MAX_STR_LEN * 2 - 1);
        return;
    }
    
    char base_domain[256] = {0};
    const char *proto_end = strstr(current_host, "://");
    if (proto_end) {
        const char *path_start = strchr(proto_end + 3, '/');
        if (path_start) {
            strncpy(base_domain, current_host, path_start - current_host);
        } else {
            strncpy(base_domain, current_host, 255);
        }
    } else {
        strncpy(base_domain, current_host, 255);
    }
    
    if (in_url[0] == '/') {
        snprintf(out_url, MAX_STR_LEN * 2, "%s%s", base_domain, in_url);
    } else {
        snprintf(out_url, MAX_STR_LEN * 2, "%s/%s", base_domain, in_url);
    }
}

// Memory write callback for catalog XML
static size_t WriteMemoryCallback(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t realsize = size * nmemb;
    struct MemoryStruct *mem = (struct MemoryStruct *)userp;

    char *ptr = realloc(mem->memory, mem->size + realsize + 1);
    if(!ptr) return 0; 
    
    mem->memory = ptr;
    memcpy(&(mem->memory[mem->size]), contents, realsize);
    mem->size += realsize;
    mem->memory[mem->size] = 0;
    
    return realsize;
}

// File write callback for downloading Books and Images
static size_t WriteFileCallback(void *ptr, size_t size, size_t nmemb, void *stream) {
    return fwrite(ptr, size, nmemb, (FILE *)stream);
}

static void SetFeedError(CURLcode result, long http_status, char *error, size_t error_size) {
    if (!error || error_size == 0) return;

    if (http_status >= 400) {
        if (http_status == 401) {
            snprintf(error, error_size, T("HTTP %ld: usuario o contraseña incorrectos."), http_status);
        } else if (http_status == 403) {
            snprintf(error, error_size, T("HTTP %ld: el servidor ha denegado el acceso."), http_status);
        } else if (http_status == 404) {
            snprintf(error, error_size, T("HTTP %ld: no se encuentra la dirección del catálogo."), http_status);
        } else if (http_status == 429) {
            snprintf(error, error_size, T("HTTP %ld: demasiadas solicitudes; inténtalo más tarde."), http_status);
        } else if (http_status >= 500) {
            snprintf(error, error_size, T("HTTP %ld: error interno del servidor."), http_status);
        } else {
            snprintf(error, error_size, T("El servidor respondió con el error HTTP %ld."), http_status);
        }
        return;
    }

    switch (result) {
        case CURLE_COULDNT_RESOLVE_HOST:
            snprintf(error, error_size, T("No se pudo encontrar el servidor (error DNS)."));
            break;
        case CURLE_COULDNT_CONNECT:
            snprintf(error, error_size, T("No se pudo establecer conexión con el servidor."));
            break;
        case CURLE_OPERATION_TIMEDOUT:
            snprintf(error, error_size, T("La conexión agotó el tiempo de espera."));
            break;
        case CURLE_SSL_CONNECT_ERROR:
            snprintf(error, error_size, T("No se pudo establecer la conexión segura HTTPS."));
            break;
        case CURLE_URL_MALFORMAT:
            snprintf(error, error_size, T("La dirección del catálogo no es válida."));
            break;
        case CURLE_UNSUPPORTED_PROTOCOL:
            snprintf(error, error_size, T("El protocolo de la dirección no es compatible."));
            break;
        case CURLE_WRITE_ERROR:
            snprintf(error, error_size, T("No se pudieron guardar los datos recibidos en memoria."));
            break;
        case CURLE_RECV_ERROR:
        case CURLE_SEND_ERROR:
        case CURLE_GOT_NOTHING:
            snprintf(error, error_size, T("La conexión se interrumpió antes de recibir el catálogo."));
            break;
        default:
            snprintf(error, error_size, T("Error de red: %s"), curl_easy_strerror(result));
            break;
    }
}

// Debug callback to log network headers
static int DebugCallback(CURL *handle, curl_infotype type, char *data, size_t size, void *userptr) {
    if (type == CURLINFO_HEADER_OUT) {
        char buf[MAX_STR_LEN];
        char *token = strtok(data, "\r\n");
        while (token != NULL) {
            snprintf(buf, sizeof(buf), ">>> SEND: %s", token);
            LogDebug(buf);
            token = strtok(NULL, "\r\n");
        }
    }
    return 0;
}

// Progress callback to update the UI
static int ProgressCallback(void *clientp, double dltotal, double dlnow, double ultotal, double ulnow) {
    if (dltotal > 0) {
        ShowDownloadProgress((long long)dltotal, (long long)dlnow);
    }
    return CheckDownloadCancel();
}

// QUOTE-AWARE HEADER PARSER
static size_t HeaderCallback(char *buffer, size_t size, size_t nitems, void *userdata) {
    size_t numbytes = size * nitems;
    char *filename = (char *)userdata;
    
    char log_buf[MAX_STR_LEN];
    char clean_buf[512] = {0};
    size_t copy_len = numbytes < 511 ? numbytes : 511;
    strncpy(clean_buf, buffer, copy_len);
    
    while(copy_len > 0 && (clean_buf[copy_len-1] == '\n' || clean_buf[copy_len-1] == '\r')) {
        clean_buf[copy_len-1] = '\0'; copy_len--;
    }
    
    if (copy_len > 0) {
        snprintf(log_buf, sizeof(log_buf), "<<< RECV: %s", clean_buf);
        LogDebug(log_buf);
    }

    if (filename && strncasecmp(buffer, "Content-Disposition:", 20) == 0) {
        char *ptr = strstr(buffer, "filename=");
        if (ptr) {
            ptr += 9; 
            if (*ptr == '"') {
                ptr++; 
                char *end = strchr(ptr, '"');
                if (end) {
                    int len = end - ptr;
                    if (len > 255) len = 255;
                    strncpy(filename, ptr, len);
                    filename[len] = '\0';
                }
            } 
            else {
                int i = 0;
                while (ptr[i] && ptr[i] != ' ' && ptr[i] != ';' && ptr[i] != '\r' && ptr[i] != '\n' && i < 255) {
                    filename[i] = ptr[i]; 
                    i++;
                }
                filename[i] = '\0';
            }
        }
    }
    return numbytes;
}

int FetchFeed(const char *url, const char *user, const char *pass,
              struct MemoryStruct *chunk, char *error, size_t error_size) {
    CURL *curl;
    CURLcode res;

    if (error && error_size > 0) error[0] = '\0';
    chunk->memory = NULL;
    chunk->size = 0;
    
    char safe_url[MAX_STR_LEN * 2] = {0};
    EnsureAbsoluteURL(url, safe_url);

    chunk->memory = malloc(1);
    if (!chunk->memory) {
        if (error && error_size > 0) {
            snprintf(error, error_size, T("No hay memoria suficiente para leer el catálogo."));
        }
        return -1;
    }

    curl = curl_easy_init();
    if (!curl) {
        free(chunk->memory);
        chunk->memory = NULL;
        if (error && error_size > 0) {
            snprintf(error, error_size, T("No se pudo iniciar la conexión HTTP."));
        }
        return -1;
    }
    
    LogDebug("================ NETWORK REQUEST START ================");
    char msg[MAX_STR_LEN]; snprintf(msg, sizeof(msg), "TARGET: [%s]", safe_url); LogDebug(msg);

    // Relaxed Network Timeouts (30 seconds)
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 30L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);

    curl_easy_setopt(curl, CURLOPT_URL, safe_url);
    curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteMemoryCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)chunk);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, APP_USER_AGENT);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(curl, CURLOPT_DEBUGFUNCTION, DebugCallback);
    curl_easy_setopt(curl, CURLOPT_VERBOSE, 1L);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, HeaderCallback);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, NULL);

    if (user && pass && strlen(user) > 0) {
        curl_easy_setopt(curl, CURLOPT_HTTPAUTH, CURLAUTH_BASIC);
        char auth[512]; snprintf(auth, sizeof(auth), "%s:%s", user, pass);
        curl_easy_setopt(curl, CURLOPT_USERPWD, auth);
    }

    res = curl_easy_perform(curl);
    long http_status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_status);
    curl_easy_cleanup(curl);
    LogDebug("================ NETWORK REQUEST END ================");

    if (res != CURLE_OK) {
        SetFeedError(res, http_status, error, error_size);
        free(chunk->memory);
        chunk->memory = NULL;
        chunk->size = 0;
        return -1;
    }
    return 0;
}

int DownloadBook(const char *url, const char *filepath, char *server_fname, const char *user, const char *pass) {
    CURL *curl;
    CURLcode res;
    FILE *fp;

    char safe_url[MAX_STR_LEN * 2] = {0};
    EnsureAbsoluteURL(url, safe_url);

    fp = fopen(filepath, "wb");
    if (!fp) return -1;

    if (server_fname) server_fname[0] = '\0'; 

    curl = curl_easy_init();
    if (!curl) { fclose(fp); return -1; }

    LogDebug("================ DOWNLOAD REQUEST START ================");
    char msg[MAX_STR_LEN]; snprintf(msg, sizeof(msg), "DOWNLOAD TARGET: [%s]", safe_url); LogDebug(msg);

    // Relaxed Network Timeouts (30 seconds)
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 30L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);

    curl_easy_setopt(curl, CURLOPT_URL, safe_url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteFileCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, fp);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, APP_USER_AGENT);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(curl, CURLOPT_DEBUGFUNCTION, DebugCallback);
    curl_easy_setopt(curl, CURLOPT_VERBOSE, 1L);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_PROGRESSFUNCTION, ProgressCallback);
    
    if (server_fname) {
        curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, HeaderCallback);
        curl_easy_setopt(curl, CURLOPT_HEADERDATA, server_fname);
    }

    if (user && pass && strlen(user) > 0) {
        curl_easy_setopt(curl, CURLOPT_HTTPAUTH, CURLAUTH_BASIC);
        char auth[512]; snprintf(auth, sizeof(auth), "%s:%s", user, pass);
        curl_easy_setopt(curl, CURLOPT_USERPWD, auth);
    }

    res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    int close_result = fclose(fp);
    LogDebug("================ DOWNLOAD REQUEST END ================");

    if (res != CURLE_OK || close_result != 0) { remove(filepath); return -1; }
    return 0;
}

int DownloadImage(const char *url, const char *filepath, const char *user, const char *pass) {
    CURL *curl;
    CURLcode res;
    FILE *fp;

    char safe_url[MAX_STR_LEN * 2] = {0};
    EnsureAbsoluteURL(url, safe_url);

    fp = fopen(filepath, "wb");
    if (!fp) return -1;

    curl = curl_easy_init();
    if (!curl) { fclose(fp); return -1; }

    // Relaxed Network Timeouts (30 seconds)
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 30L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);

    curl_easy_setopt(curl, CURLOPT_URL, safe_url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteFileCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, fp);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, APP_USER_AGENT);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);

    if (user && pass && strlen(user) > 0) {
        curl_easy_setopt(curl, CURLOPT_HTTPAUTH, CURLAUTH_BASIC);
        char auth[512]; snprintf(auth, sizeof(auth), "%s:%s", user, pass);
        curl_easy_setopt(curl, CURLOPT_USERPWD, auth);
    }

    res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    fclose(fp);

    if (res != CURLE_OK) { remove(filepath); return -1; }
    return 0;
}
