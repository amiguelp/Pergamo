#include "opds_app.h"
#include <curl/curl.h>
#include <json-c/json.h>
#include <libxml/uri.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

extern void LogDebug(const char *msg);

#define EBIBLIO_AUTH_DOCUMENT_URL "https://madrid.ebiblio.es/v1/sign_in.opds2"
#define EBIBLIO_TOKEN_CAPACITY 4096

static char ebiblio_auth_url[MAX_STR_LEN * 2] = {0};
static char ebiblio_access_token[EBIBLIO_TOKEN_CAPACITY] = {0};
static char ebiblio_token_user[MAX_STR_LEN] = {0};
static time_t ebiblio_token_expiry = 0;

static int IsTrustedEbiblioURL(const char *url) {
    if (!url || strncmp(url, "https://", 8) != 0) return 0;

    const char *host = url + 8;
    const char *end = host;
    while (*end && *end != '/' && *end != '?' && *end != '#' && *end != ':') end++;

    return (size_t)(end - host) == sizeof("madrid.ebiblio.es") - 1 &&
           strncasecmp(host, "madrid.ebiblio.es", sizeof("madrid.ebiblio.es") - 1) == 0;
}

int IsEbiblioURL(const char *url) {
    if (!url) return 0;
    const char *scheme = strstr(url, "://");
    if (!scheme) return 0;
    return IsTrustedEbiblioURL(url);
}

static size_t WriteMemoryCallback(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t bytes = size * nmemb;
    struct MemoryStruct *memory = (struct MemoryStruct *)userp;
    char *new_memory = realloc(memory->memory, memory->size + bytes + 1);
    if (!new_memory) return 0;

    memory->memory = new_memory;
    memcpy(memory->memory + memory->size, contents, bytes);
    memory->size += bytes;
    memory->memory[memory->size] = '\0';
    return bytes;
}

static void SetNetworkError(CURLcode result, long http_status,
                            char *error, size_t error_size) {
    if (!error || error_size == 0) return;

    if (http_status >= 400) {
        if (http_status == 401) {
            snprintf(error, error_size, T("eBiblio no ha aceptado la sesión."));
        } else if (http_status == 403) {
            snprintf(error, error_size, T("eBiblio ha denegado la operación."));
        } else if (http_status == 404) {
            snprintf(error, error_size,
                     "%s",
                     T("eBiblio no encuentra el recurso solicitado (HTTP 404). "
                     "Actualiza Préstamos y reservas para obtener los enlaces vigentes; "
                     "si tu reserva está disponible, utiliza Aceptar préstamo desde su ficha."));
        } else if (http_status == 409) {
            snprintf(error, error_size,
                     "%s",
                     T("eBiblio no permite la operación en el estado actual (HTTP 409). "
                     "Puede que la ficha haya cambiado o no queden plazas de reserva; "
                     "actualiza Préstamos y reservas y vuelve a intentarlo."));
        } else if (http_status >= 500) {
            snprintf(error, error_size, T("eBiblio tiene un error temporal (HTTP %ld)."), http_status);
        } else {
            snprintf(error, error_size, T("eBiblio respondió con HTTP %ld."), http_status);
        }
        return;
    }

    switch (result) {
        case CURLE_COULDNT_RESOLVE_HOST:
            snprintf(error, error_size, T("No se pudo encontrar madrid.ebiblio.es (error DNS)."));
            break;
        case CURLE_COULDNT_CONNECT:
            snprintf(error, error_size, T("No se pudo conectar con eBiblio Madrid."));
            break;
        case CURLE_OPERATION_TIMEDOUT:
            snprintf(error, error_size, T("La conexión con eBiblio agotó el tiempo de espera."));
            break;
        case CURLE_SSL_CONNECT_ERROR:
            snprintf(error, error_size, T("No se pudo establecer una conexión HTTPS segura con eBiblio."));
            break;
        default:
            snprintf(error, error_size, T("Error de red: %s"), curl_easy_strerror(result));
            break;
    }
}

static int RequestRaw(const char *url, const char *method, const char *body,
                      const char *access_token, struct MemoryStruct *response,
                      long *http_status, char *error, size_t error_size) {
    CURL *curl = curl_easy_init();
    if (!curl) {
        if (error && error_size) snprintf(error, error_size, T("No se pudo iniciar la conexión HTTP."));
        return -1;
    }

    response->memory = malloc(1);
    response->size = 0;
    if (!response->memory) {
        curl_easy_cleanup(curl);
        if (error && error_size) snprintf(error, error_size, T("No hay memoria suficiente para consultar eBiblio."));
        return -1;
    }
    response->memory[0] = '\0';

    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Accept: application/opds+json, application/json");
    if (method && (strcmp(method, "POST") == 0 ||
                   strcmp(method, "PUT") == 0)) {
        headers = curl_slist_append(headers, "Content-Type: application/x-www-form-urlencoded");
    }
    char authorization[EBIBLIO_TOKEN_CAPACITY + 32];
    if (access_token && access_token[0]) {
        snprintf(authorization, sizeof(authorization), "Authorization: Bearer %s", access_token);
        headers = curl_slist_append(headers, authorization);
    }

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 30L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, APP_USER_AGENT);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteMemoryCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, response);
    if (method && strcmp(method, "POST") == 0) {
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body ? body : "");
    } else if (method && strcmp(method, "PUT") == 0) {
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PUT");
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body ? body : "");
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, body ? (long)strlen(body) : 0L);
    } else if (method && strcmp(method, "GET") != 0) {
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method);
    }

    CURLcode result = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    if (http_status) *http_status = status;
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    int action_redirect = method &&
                          (strcmp(method, "POST") == 0 ||
                           strcmp(method, "PUT") == 0 ||
                           strcmp(method, "DELETE") == 0) &&
                          url != NULL && url != ebiblio_auth_url &&
                          status >= 300 && status < 400;
    if (method && strcmp(method, "GET") != 0) {
        char log_line[MAX_STR_LEN * 3];
        snprintf(log_line, sizeof(log_line), "eBiblio %s %s -> HTTP %ld (curl %d): %.300s",
                 method, url ? url : "", status, (int)result,
                 response->memory ? response->memory : "");
        LogDebug(log_line);
    }
    if (result != CURLE_OK ||
        ((status < 200 || status >= 300) && !action_redirect)) {
        SetNetworkError(result, status, error, error_size);
        free(response->memory);
        response->memory = NULL;
        response->size = 0;
        return -1;
    }
    return 0;
}

static int UrlEncodeFormValue(const char *source, char *destination, size_t capacity) {
    static const char hex[] = "0123456789ABCDEF";
    size_t used = 0;
    if (!source || !destination || capacity == 0) return -1;

    for (const unsigned char *p = (const unsigned char *)source; *p; p++) {
        if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
            (*p >= '0' && *p <= '9') || *p == '-' || *p == '_' ||
            *p == '.' || *p == '~') {
            if (used + 1 >= capacity) return -1;
            destination[used++] = (char)*p;
        } else {
            if (used + 3 >= capacity) return -1;
            destination[used++] = '%';
            destination[used++] = hex[*p >> 4];
            destination[used++] = hex[*p & 15];
        }
    }
    destination[used] = '\0';
    return 0;
}

static int DiscoverAuthenticationURL(char *error, size_t error_size) {
    struct MemoryStruct response = {NULL, 0};
    long status = 0;
    if (RequestRaw(EBIBLIO_AUTH_DOCUMENT_URL, "GET", NULL, NULL,
                   &response, &status, error, error_size) != 0) {
        return -1;
    }

    struct json_object *root = json_tokener_parse(response.memory);
    free(response.memory);
    if (!root) {
        if (error && error_size) snprintf(error, error_size, T("eBiblio devolvió un documento de acceso no válido."));
        return -1;
    }

    struct json_object *flows = NULL;
    int found = 0;
    if (json_object_object_get_ex(root, "authentication", &flows) &&
        json_object_is_type(flows, json_type_array)) {
        for (size_t i = 0; i < json_object_array_length(flows); i++) {
            struct json_object *flow = json_object_array_get_idx(flows, i);
            struct json_object *type = NULL;
            struct json_object *links = NULL;
            if (!json_object_object_get_ex(flow, "type", &type) ||
                !json_object_is_type(type, json_type_string) ||
                !strstr(json_object_get_string(type), "auth/oauth/password") ||
                !json_object_object_get_ex(flow, "links", &links) ||
                !json_object_is_type(links, json_type_array)) {
                continue;
            }

            for (size_t j = 0; j < json_object_array_length(links); j++) {
                struct json_object *link = json_object_array_get_idx(links, j);
                struct json_object *rel = NULL;
                struct json_object *href = NULL;
                if (json_object_object_get_ex(link, "rel", &rel) &&
                    json_object_is_type(rel, json_type_string) &&
                    strcmp(json_object_get_string(rel), "authenticate") == 0 &&
                    json_object_object_get_ex(link, "href", &href) &&
                    json_object_is_type(href, json_type_string)) {
                    const char *candidate = json_object_get_string(href);
                    if (IsTrustedEbiblioURL(candidate)) {
                        strncpy(ebiblio_auth_url, candidate, sizeof(ebiblio_auth_url) - 1);
                        ebiblio_auth_url[sizeof(ebiblio_auth_url) - 1] = '\0';
                        found = 1;
                        break;
                    }
                }
            }
            if (found) break;
        }
    }
    json_object_put(root);

    if (!found && error && error_size) {
        snprintf(error, error_size, T("eBiblio no ofrece el acceso OAuth esperado."));
    }
    return found ? 0 : -1;
}

static int Authenticate(const char *user, const char *pass,
                        char *error, size_t error_size) {
    if (!user || !user[0] || !pass || !pass[0]) {
        if (error && error_size) snprintf(error, error_size, T("Configura el carné y la contraseña de eBiblio en los datos del servidor."));
        return -1;
    }

    if (!ebiblio_auth_url[0] && DiscoverAuthenticationURL(error, error_size) != 0) {
        return -1;
    }

    char encoded_user[MAX_STR_LEN * 3];
    char encoded_pass[MAX_STR_LEN * 3];
    if (UrlEncodeFormValue(user, encoded_user, sizeof(encoded_user)) != 0 ||
        UrlEncodeFormValue(pass, encoded_pass, sizeof(encoded_pass)) != 0) {
        if (error && error_size) snprintf(error, error_size, T("El usuario o la contraseña de eBiblio son demasiado largos."));
        return -1;
    }

    char body[MAX_STR_LEN * 6 + 64];
    snprintf(body, sizeof(body), "grant_type=password&username=%s&password=%s",
             encoded_user, encoded_pass);

    struct MemoryStruct response = {NULL, 0};
    long status = 0;
    if (RequestRaw(ebiblio_auth_url, "POST", body, NULL,
                   &response, &status, error, error_size) != 0) {
        if (status == 400 || status == 401) {
            if (error && error_size) snprintf(error, error_size, T("eBiblio rechazó las credenciales. Comprueba el número de carné y la contraseña."));
        }
        return -1;
    }

    struct json_object *root = json_tokener_parse(response.memory);
    free(response.memory);
    struct json_object *token = NULL;
    if (!root || !json_object_object_get_ex(root, "access_token", &token) ||
        !json_object_is_type(token, json_type_string)) {
        if (root) json_object_put(root);
        if (error && error_size) snprintf(error, error_size, T("eBiblio no devolvió un token de acceso válido."));
        return -1;
    }

    const char *token_value = json_object_get_string(token);
    if (strlen(token_value) >= sizeof(ebiblio_access_token)) {
        json_object_put(root);
        if (error && error_size) snprintf(error, error_size, T("El token de eBiblio supera el tamaño admitido."));
        return -1;
    }
    strcpy(ebiblio_access_token, token_value);
    strncpy(ebiblio_token_user, user, sizeof(ebiblio_token_user) - 1);
    ebiblio_token_user[sizeof(ebiblio_token_user) - 1] = '\0';

    struct json_object *expires = NULL;
    int expires_in = 3600;
    if (json_object_object_get_ex(root, "expires_in", &expires) &&
        json_object_is_type(expires, json_type_int)) {
        expires_in = json_object_get_int(expires);
    }
    if (expires_in < 60) expires_in = 60;
    ebiblio_token_expiry = time(NULL) + expires_in;
    json_object_put(root);
    return 0;
}

static void ClearToken(void) {
    memset(ebiblio_access_token, 0, sizeof(ebiblio_access_token));
    ebiblio_token_user[0] = '\0';
    ebiblio_token_expiry = 0;
}

static int EnsureToken(const char *user, const char *pass,
                       char *error, size_t error_size) {
    if (ebiblio_access_token[0] &&
        strcmp(ebiblio_token_user, user ? user : "") == 0 &&
        time(NULL) + 30 < ebiblio_token_expiry) {
        return 0;
    }
    ClearToken();
    return Authenticate(user, pass, error, error_size);
}

static int RequestAuthorized(const char *url, const char *method, const char *body,
                             const char *user, const char *pass,
                             struct MemoryStruct *response, char *error,
                             size_t error_size) {
    if (!IsTrustedEbiblioURL(url)) {
        if (error && error_size) snprintf(error, error_size, T("Se rechazó una dirección ajena a eBiblio Madrid."));
        return -1;
    }
    if (EnsureToken(user, pass, error, error_size) != 0) return -1;

    long status = 0;
    int result = RequestRaw(url, method, body, ebiblio_access_token,
                            response, &status, error, error_size);
    if (result == 0 || status != 401) return result;

    ClearToken();
    if (Authenticate(user, pass, error, error_size) != 0) return -1;
    return RequestRaw(url, method, body, ebiblio_access_token,
                      response, NULL, error, error_size);
}

int EbiblioFetchFeed(const char *url, const char *user, const char *pass,
                     int require_auth, struct MemoryStruct *chunk,
                     char *error, size_t error_size) {
    if (error && error_size) error[0] = '\0';
    if (!IsTrustedEbiblioURL(url)) {
        if (error && error_size) snprintf(error, error_size, T("La dirección no pertenece a eBiblio Madrid."));
        return -1;
    }

    if (require_auth) {
        return RequestAuthorized(url, "GET", NULL, user, pass,
                                 chunk, error, error_size);
    }

    long status = 0;
    int result = RequestRaw(url, "GET", NULL, NULL, chunk,
                            &status, error, error_size);
    if (result == 0 || status != 401) return result;
    return RequestAuthorized(url, "GET", NULL, user, pass,
                             chunk, error, error_size);
}

int EbiblioPostAction(const char *url, const char *user, const char *pass,
                      char *error, size_t error_size) {
    struct MemoryStruct response = {NULL, 0};
    int result = RequestAuthorized(url, "POST", "", user, pass,
                                   &response, error, error_size);
    free(response.memory);
    return result;
}

int EbiblioPutAction(const char *url, const char *user, const char *pass,
                     char *error, size_t error_size) {
    struct MemoryStruct response = {NULL, 0};
    int result = RequestAuthorized(url, "PUT", NULL, user, pass,
                                   &response, error, error_size);
    free(response.memory);
    return result;
}

int EbiblioDeleteAction(const char *url, const char *user, const char *pass,
                        char *error, size_t error_size) {
    struct MemoryStruct response = {NULL, 0};
    int result = RequestAuthorized(url, "DELETE", NULL, user, pass,
                                   &response, error, error_size);
    free(response.memory);
    return result;
}

static size_t WriteFileCallback(void *contents, size_t size, size_t nmemb, void *userp) {
    return fwrite(contents, size, nmemb, (FILE *)userp);
}

typedef struct {
    char location[MAX_STR_LEN * 2];
    char filename[256];
} EbiblioDownloadHeaders;

static size_t RedirectHeaderCallback(char *buffer, size_t size, size_t nitems, void *userdata) {
    size_t bytes = size * nitems;
    EbiblioDownloadHeaders *headers = (EbiblioDownloadHeaders *)userdata;
    if (bytes > 9 && strncasecmp(buffer, "Location:", 9) == 0) {
        size_t start = 9;
        while (start < bytes && (buffer[start] == ' ' || buffer[start] == '\t')) start++;
        size_t length = bytes - start;
        while (length > 0 && (buffer[start + length - 1] == '\r' ||
                              buffer[start + length - 1] == '\n')) length--;
        if (length >= sizeof(headers->location)) length = sizeof(headers->location) - 1;
        memcpy(headers->location, buffer + start, length);
        headers->location[length] = '\0';
    } else if (headers && bytes > 20 &&
               strncasecmp(buffer, "Content-Disposition:", 20) == 0) {
        char line[1024] = {0};
        size_t length = bytes < sizeof(line) - 1 ? bytes : sizeof(line) - 1;
        memcpy(line, buffer, length);

        for (size_t i = 0; i + 9 < length; i++) {
            if (strncasecmp(line + i, "filename=", 9) != 0) continue;
            char *value = line + i + 9;
            char *end;
            if (*value == '"') {
                value++;
                end = strchr(value, '"');
            } else {
                end = value;
                while (*end && *end != ';' && *end != '\r' && *end != '\n' &&
                       *end != ' ' && *end != '\t') end++;
            }
            if (!end || end <= value) break;
            size_t filename_length = (size_t)(end - value);
            if (filename_length >= sizeof(headers->filename)) {
                filename_length = sizeof(headers->filename) - 1;
            }
            memcpy(headers->filename, value, filename_length);
            headers->filename[filename_length] = '\0';
            break;
        }
    }
    return bytes;
}

static const char *JsonString(struct json_object *object, const char *key);
static int RelContains(struct json_object *rel, const char *needle);

int EbiblioDownloadWithInfo(const char *url, const char *filepath,
                            const char *user, const char *pass,
                            char *filename, size_t filename_size,
                            char *content_type, size_t content_type_size,
                            char *error, size_t error_size) {
    char current_url[MAX_STR_LEN * 2];
    if (error && error_size) error[0] = '\0';
    if (filename && filename_size) filename[0] = '\0';
    if (content_type && content_type_size) content_type[0] = '\0';
    if (!url || !filepath || strncmp(url, "https://", 8) != 0 ||
        strlen(url) >= sizeof(current_url)) {
        if (error && error_size) snprintf(error, error_size, T("La dirección de descarga no es válida o no usa HTTPS."));
        return -1;
    }
    strcpy(current_url, url);

    for (int redirect_count = 0; redirect_count <= 5; redirect_count++) {
        int trusted_host = IsTrustedEbiblioURL(current_url);
        if (trusted_host && EnsureToken(user, pass, error, error_size) != 0) return -1;

        FILE *file = fopen(filepath, "wb");
        if (!file) {
            if (error && error_size) snprintf(error, error_size, T("No se pudo crear el archivo de descarga."));
            return -1;
        }

        CURL *curl = curl_easy_init();
        if (!curl) {
            fclose(file);
            remove(filepath);
            if (error && error_size) snprintf(error, error_size, T("No se pudo iniciar la descarga."));
            return -1;
        }

        struct curl_slist *headers = NULL;
        headers = curl_slist_append(headers, "Accept: */*");
        char authorization[EBIBLIO_TOKEN_CAPACITY + 32];
        if (trusted_host) {
            snprintf(authorization, sizeof(authorization), "Authorization: Bearer %s",
                     ebiblio_access_token);
            headers = curl_slist_append(headers, authorization);
        }

        EbiblioDownloadHeaders response_headers = {{0}, {0}};
        curl_easy_setopt(curl, CURLOPT_URL, current_url);
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 30L);
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 30L);
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
        curl_easy_setopt(curl, CURLOPT_USERAGENT, APP_USER_AGENT);
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteFileCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, file);
        curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, RedirectHeaderCallback);
        curl_easy_setopt(curl, CURLOPT_HEADERDATA, &response_headers);

        CURLcode transfer = curl_easy_perform(curl);
        long status = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
        char response_content_type[128] = {0};
        char *curl_content_type = NULL;
        if (curl_easy_getinfo(curl, CURLINFO_CONTENT_TYPE, &curl_content_type) == CURLE_OK &&
            curl_content_type) {
            strncpy(response_content_type, curl_content_type, sizeof(response_content_type) - 1);
        }
        int close_result = fclose(file);
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);

        if (transfer != CURLE_OK || close_result != 0) {
            remove(filepath);
            SetNetworkError(transfer, status, error, error_size);
            return -1;
        }

        if (status == 401 && trusted_host && redirect_count < 5) {
            remove(filepath);
            ClearToken();
            if (Authenticate(user, pass, error, error_size) != 0) return -1;
            continue;
        }

        if (status >= 200 && status < 300) {
            if (filename && filename_size && response_headers.filename[0]) {
                strncpy(filename, response_headers.filename, filename_size - 1);
                filename[filename_size - 1] = '\0';
            }
            if (content_type && content_type_size && response_content_type[0]) {
                strncpy(content_type, response_content_type, content_type_size - 1);
                content_type[content_type_size - 1] = '\0';
            }
            return 0;
        }
        if (status >= 300 && status < 400 && response_headers.location[0]) {
            remove(filepath);
            xmlChar *resolved = xmlBuildURI((const xmlChar *)response_headers.location,
                                            (const xmlChar *)current_url);
            if (!resolved) {
                if (error && error_size) snprintf(error, error_size, T("eBiblio devolvió una redirección no válida."));
                return -1;
            }
            if (strlen((const char *)resolved) >= sizeof(current_url) ||
                strncmp((const char *)resolved, "https://", 8) != 0) {
                xmlFree(resolved);
                if (error && error_size) snprintf(error, error_size, T("La redirección de descarga no es segura."));
                return -1;
            }
            strncpy(current_url, (const char *)resolved, sizeof(current_url) - 1);
            current_url[sizeof(current_url) - 1] = '\0';
            xmlFree(resolved);
            continue;
        }

        remove(filepath);
        SetNetworkError(CURLE_OK, status, error, error_size);
        return -1;
    }

    remove(filepath);
    if (error && error_size) snprintf(error, error_size, T("La descarga superó el límite de redirecciones."));
    return -1;
}

int EbiblioDownload(const char *url, const char *filepath,
                    const char *user, const char *pass,
                    char *error, size_t error_size) {
    return EbiblioDownloadWithInfo(url, filepath, user, pass,
                                   NULL, 0, NULL, 0, error, error_size);
}

int EbiblioValidateLicenseFile(const char *filepath, char *error, size_t error_size) {
    if (error && error_size) error[0] = '\0';
    if (!filepath) {
        if (error && error_size) snprintf(error, error_size, T("No se indicó el archivo de licencia."));
        return -1;
    }

    FILE *file = fopen(filepath, "rb");
    if (!file) {
        if (error && error_size) snprintf(error, error_size, T("No se pudo leer el archivo de licencia."));
        return -1;
    }
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        if (error && error_size) snprintf(error, error_size, T("No se pudo comprobar el archivo de licencia."));
        return -1;
    }
    long file_size = ftell(file);
    if (file_size <= 0 || file_size > 1024 * 1024 || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        if (error && error_size) snprintf(error, error_size, T("El archivo LCP está vacío o tiene un tamaño no válido."));
        return -1;
    }

    char *contents = malloc((size_t)file_size + 1);
    if (!contents) {
        fclose(file);
        if (error && error_size) snprintf(error, error_size, T("No hay memoria suficiente para validar la licencia LCP."));
        return -1;
    }
    size_t read_size = fread(contents, 1, (size_t)file_size, file);
    fclose(file);
    if (read_size != (size_t)file_size) {
        free(contents);
        if (error && error_size) snprintf(error, error_size, T("No se pudo leer el archivo de licencia completo."));
        return -1;
    }
    contents[file_size] = '\0';

    struct json_object *root = json_tokener_parse(contents);
    free(contents);
    if (!root || !json_object_is_type(root, json_type_object)) {
        if (root) json_object_put(root);
        if (error && error_size) snprintf(error, error_size, T("eBiblio no devolvió una licencia LCP JSON válida."));
        return -1;
    }

    struct json_object *encryption = NULL;
    struct json_object *links = NULL;
    struct json_object *signature = NULL;
    int has_core_fields =
        JsonString(root, "id") && JsonString(root, "issued") &&
        JsonString(root, "provider") &&
        json_object_object_get_ex(root, "encryption", &encryption) &&
        json_object_is_type(encryption, json_type_object) &&
        json_object_object_get_ex(root, "links", &links) &&
        json_object_is_type(links, json_type_array) &&
        json_object_object_get_ex(root, "signature", &signature);
    int has_publication = 0;
    if (has_core_fields) {
        for (size_t i = 0; i < json_object_array_length(links); i++) {
            struct json_object *link = json_object_array_get_idx(links, i);
            struct json_object *rel = NULL;
            if (json_object_object_get_ex(link, "rel", &rel) &&
                RelContains(rel, "publication") &&
                JsonString(link, "href")) {
                has_publication = 1;
                break;
            }
        }
    }
    json_object_put(root);

    if (!has_core_fields || !has_publication) {
        if (error && error_size) {
            snprintf(error, error_size,
                     T("El archivo descargado no contiene una licencia LCP completa con enlace al EPUB."));
        }
        return -1;
    }
    return 0;
}

static const char *JsonString(struct json_object *object, const char *key) {
    struct json_object *value = NULL;
    if (!object || !json_object_object_get_ex(object, key, &value) ||
        !json_object_is_type(value, json_type_string)) return NULL;
    return json_object_get_string(value);
}

static int RelContains(struct json_object *rel, const char *needle) {
    if (!rel || !needle) return 0;
    if (json_object_is_type(rel, json_type_string)) {
        return strstr(json_object_get_string(rel), needle) != NULL;
    }
    if (json_object_is_type(rel, json_type_array)) {
        for (size_t i = 0; i < json_object_array_length(rel); i++) {
            struct json_object *value = json_object_array_get_idx(rel, i);
            if (json_object_is_type(value, json_type_string) &&
                strstr(json_object_get_string(value), needle)) return 1;
        }
    }
    return 0;
}

static void AddReturnURLFromLoanURL(OPDSEntry *entry, const char *url) {
    if (!entry || entry->ebiblio_return_url[0] || !url) return;

    if (!IsTrustedEbiblioURL(url)) return;
    const char *loan = strstr(url, "/loans/");
    if (!loan) return;
    loan += strlen("/loans/");
    const char *end = loan;
    while (*end && *end != '/' && *end != '?' && *end != '#' && *end != '.') end++;
    size_t loan_id_length = (size_t)(end - loan);
    if (loan_id_length == 0 || loan_id_length >= 128) return;
    for (size_t i = 0; i < loan_id_length; i++) {
        if (!isalnum((unsigned char)loan[i]) && loan[i] != '-' && loan[i] != '_') return;
    }

    /* Keep the original prefix (e.g. https://madrid.ebiblio.es/v1/loans/ID):
       every eBiblio API path lives under /v1/, so building the URL from the
       host root ("/loans/ID/return") pointed at a non-existent endpoint. */
    int written = snprintf(entry->ebiblio_return_url,
                           sizeof(entry->ebiblio_return_url),
                           "%.*s/return", (int)(end - url), url);
    if (written > 0 && (size_t)written < sizeof(entry->ebiblio_return_url)) {
        entry->ebiblio_return_uses_put = 1;
    } else {
        entry->ebiblio_return_url[0] = '\0';
    }
}

static void ResolveEbiblioURL(const char *base_url, const char *href,
                              char *output, size_t output_size) {
    if (!href || !output || output_size == 0) return;
    xmlChar *resolved = xmlBuildURI((const xmlChar *)href, (const xmlChar *)base_url);
    const char *url = resolved ? (const char *)resolved : href;
    strncpy(output, url, output_size - 1);
    output[output_size - 1] = '\0';
    if (resolved) xmlFree(resolved);
}

static void CopyAuthor(struct json_object *metadata, char *output, size_t output_size) {
    struct json_object *authors = NULL;
    if (!json_object_object_get_ex(metadata, "author", &authors)) return;

    struct json_object *author = authors;
    if (json_object_is_type(authors, json_type_array)) {
        if (json_object_array_length(authors) == 0) return;
        author = json_object_array_get_idx(authors, 0);
    }

    if (json_object_is_type(author, json_type_string)) {
        strncpy(output, json_object_get_string(author), output_size - 1);
    } else {
        const char *name = JsonString(author, "name");
        if (name) strncpy(output, name, output_size - 1);
    }
    output[output_size - 1] = '\0';
}

static int TextContainsIgnoreCase(const char *text, const char *needle) {
    if (!text || !needle || !needle[0]) return 0;
    size_t needle_length = strlen(needle);
    for (const char *cursor = text; *cursor; cursor++) {
        if (strncasecmp(cursor, needle, needle_length) == 0) return 1;
    }
    return 0;
}

static void CopyPublicationLinks(struct json_object *publication,
                                 const char *base_url, OPDSEntry *entry) {
    struct json_object *links = NULL;
    if (!json_object_object_get_ex(publication, "links", &links) ||
        !json_object_is_type(links, json_type_array)) return;

    for (size_t i = 0; i < json_object_array_length(links); i++) {
        struct json_object *link = json_object_array_get_idx(links, i);
        struct json_object *rel = NULL;
        if (!json_object_object_get_ex(link, "rel", &rel)) continue;
        const char *href = JsonString(link, "href");
        const char *title = JsonString(link, "title");
        if (!href) continue;

        if (RelContains(rel, "return") ||
            TextContainsIgnoreCase(title, "return") ||
            TextContainsIgnoreCase(title, "devolver") ||
            TextContainsIgnoreCase(title, "devolución") ||
            TextContainsIgnoreCase(href, "/return")) {
            ResolveEbiblioURL(base_url, href, entry->ebiblio_return_url,
                              sizeof(entry->ebiblio_return_url));
            entry->ebiblio_return_uses_put = 0;
        } else if (RelContains(rel, "accept") ||
                   TextContainsIgnoreCase(title, "accept") ||
                   TextContainsIgnoreCase(title, "aceptar") ||
                   TextContainsIgnoreCase(title, "prestar")) {
            ResolveEbiblioURL(base_url, href, entry->ebiblio_accept_url,
                              sizeof(entry->ebiblio_accept_url));
        } else if (RelContains(rel, "cancel") ||
                   RelContains(rel, "remove-hold") ||
                   RelContains(rel, "hold/delete") ||
                   RelContains(rel, "reservation/delete") ||
                   TextContainsIgnoreCase(title, "cancel") ||
                   TextContainsIgnoreCase(title, "anular") ||
                   TextContainsIgnoreCase(title, "cancelación") ||
                   TextContainsIgnoreCase(title, "quitar reserva") ||
                   TextContainsIgnoreCase(title, "retirar reserva") ||
                   TextContainsIgnoreCase(href, "/cancel")) {
            ResolveEbiblioURL(base_url, href, entry->ebiblio_cancel_url,
                              sizeof(entry->ebiblio_cancel_url));
            entry->ebiblio_cancel_uses_delete = 0;
        } else if (RelContains(rel, "acquisition/borrow") ||
                   RelContains(rel, "rel/borrow") ||
                   RelContains(rel, "borrow") ||
                   RelContains(rel, "acquisition/hold") ||
                   RelContains(rel, "rel/hold") ||
                   RelContains(rel, "hold") ||
                   TextContainsIgnoreCase(title, "borrow") ||
                   TextContainsIgnoreCase(title, "hold") ||
                   TextContainsIgnoreCase(title, "reservar") ||
                   TextContainsIgnoreCase(title, "pedir préstamo") ||
                   TextContainsIgnoreCase(href, "/borrow") ||
                   TextContainsIgnoreCase(href, "/hold")) {
            ResolveEbiblioURL(base_url, href, entry->ebiblio_borrow_url,
                              sizeof(entry->ebiblio_borrow_url));

            struct json_object *properties = NULL;
            struct json_object *availability = NULL;
            struct json_object *state = NULL;
            if (json_object_object_get_ex(link, "properties", &properties)) {
                struct json_object *holds = NULL;
                struct json_object *value = NULL;
                struct json_object *cancellable = NULL;
                int has_availability =
                    json_object_object_get_ex(properties, "availability", &availability) &&
                    json_object_is_type(availability, json_type_object);
                if (has_availability) {
                    if (json_object_object_get_ex(availability, "state", &state) &&
                        json_object_is_type(state, json_type_string)) {
                        strncpy(entry->ebiblio_status, json_object_get_string(state),
                                sizeof(entry->ebiblio_status) - 1);
                        entry->ebiblio_available =
                            strcmp(json_object_get_string(state), "available") == 0;
                    }
                    const char *until = JsonString(availability, "until");
                    if (until) {
                        strncpy(entry->ebiblio_availability_until, until,
                                sizeof(entry->ebiblio_availability_until) - 1);
                    }
                    json_object_object_get_ex(availability, "holds", &holds);
                }
                if (!holds) {
                    json_object_object_get_ex(properties, "holds", &holds);
                }
                if (holds && json_object_is_type(holds, json_type_object)) {
                    if (json_object_object_get_ex(holds, "total", &value) &&
                        json_object_is_type(value, json_type_int)) {
                        entry->ebiblio_holds_total = json_object_get_int(value);
                    }
                    if (json_object_object_get_ex(holds, "position", &value) &&
                        json_object_is_type(value, json_type_int)) {
                        entry->ebiblio_hold_position = json_object_get_int(value);
                    }
                }
                if (!json_object_object_get_ex(properties, "cancellable", &cancellable) &&
                    has_availability) {
                    json_object_object_get_ex(availability, "cancellable", &cancellable);
                }
                if (cancellable && json_object_is_type(cancellable, json_type_boolean)) {
                    entry->ebiblio_cancellable =
                        json_object_get_boolean(cancellable) ? 1 : -1;
                    if (entry->ebiblio_cancellable > 0 &&
                        !entry->ebiblio_cancel_url[0]) {
                        strncpy(entry->ebiblio_cancel_url, entry->ebiblio_borrow_url,
                                sizeof(entry->ebiblio_cancel_url) - 1);
                        entry->ebiblio_cancel_uses_delete = 1;
                    }
                    AddReturnURLFromLoanURL(entry, entry->ebiblio_self_url);
                }
            }
        } else if (RelContains(rel, "sample")) {
            continue;
        } else if (RelContains(rel, "acquisition") || RelContains(rel, "download")) {
            const char *type = JsonString(link, "type");
            if (type && (strstr(type, "opds-publication+json") ||
                         strstr(type, "application/opds+json"))) continue;
            const char *format_label = NULL;
            if (type && strstr(type, "adept")) {
                format_label = "ACSM";
            } else if (type && strstr(type, "lcp")) {
                format_label = "LCP";
            } else if (type && strstr(type, "epub")) {
                format_label = "EPUB";
            } else if (type && strstr(type, "pdf")) {
                format_label = "PDF";
            }
            if (format_label && entry->format_count < MAX_FORMATS) {
                char resolved_url[MAX_STR_LEN];
                ResolveEbiblioURL(base_url, href, resolved_url, sizeof(resolved_url));
                if (strcmp(format_label, "LCP") == 0) {
                    AddReturnURLFromLoanURL(entry, resolved_url);
                }
                int duplicate = 0;
                for (int j = 0; j < entry->format_count; j++) {
                    if (strcmp(entry->formats[j].url, resolved_url) == 0 ||
                        (strcmp(format_label, "LCP") == 0 &&
                         strcmp(entry->formats[j].label, "LCP") == 0)) {
                        duplicate = 1;
                        break;
                    }
                }
                if (!duplicate) {
                    BookFormat *format = &entry->formats[entry->format_count];
                    strncpy(format->url, resolved_url, sizeof(format->url) - 1);
                    format->url[sizeof(format->url) - 1] = '\0';
                    strncpy(format->label, format_label, sizeof(format->label) - 1);
                    format->label[sizeof(format->label) - 1] = '\0';
                    entry->format_count++;
                }
            }
        }

        if (RelContains(rel, "self")) {
            char self_url[MAX_STR_LEN];
            ResolveEbiblioURL(base_url, href, self_url, sizeof(self_url));
            if (!entry->ebiblio_self_url[0]) {
                strncpy(entry->ebiblio_self_url, self_url,
                        sizeof(entry->ebiblio_self_url) - 1);
            }
            AddReturnURLFromLoanURL(entry, self_url);
        }
    }

    int queued = strcmp(entry->ebiblio_status, "reserved") == 0 ||
                 strcmp(entry->ebiblio_status, "ready") == 0 ||
                 entry->ebiblio_hold_position > 0;
    if (queued && !entry->ebiblio_accept_url[0] &&
        strcmp(entry->ebiblio_status, "ready") == 0) {
        strncpy(entry->ebiblio_accept_url, entry->ebiblio_borrow_url,
                sizeof(entry->ebiblio_accept_url) - 1);
    }
    if (queued && !entry->ebiblio_cancel_url[0] &&
        entry->ebiblio_cancellable >= 0 && entry->ebiblio_borrow_url[0]) {
        strncpy(entry->ebiblio_cancel_url, entry->ebiblio_borrow_url,
                sizeof(entry->ebiblio_cancel_url) - 1);
        entry->ebiblio_cancel_uses_delete = 1;
    }
}

static int AddNavigationEntries(struct json_object *array, const char *base_url,
                                int max_entries) {
    if (!array || !json_object_is_type(array, json_type_array)) return 0;
    int added = 0;
    for (size_t i = 0; i < json_object_array_length(array) && entry_count < max_entries; i++) {
        struct json_object *link = json_object_array_get_idx(array, i);
        const char *title = JsonString(link, "title");
        const char *href = JsonString(link, "href");
        if (!title || !href) continue;

        OPDSEntry *entry = &current_entries[entry_count++];
        memset(entry, 0, sizeof(*entry));
        strncpy(entry->title, title, sizeof(entry->title) - 1);
        ResolveEbiblioURL(base_url, href, entry->nav_url, sizeof(entry->nav_url));
        added++;
    }
    return added;
}

static int AddPublicationEntries(struct json_object *array, const char *base_url,
                                 int max_entries) {
    if (!array || !json_object_is_type(array, json_type_array)) return 0;
    int added = 0;
    for (size_t i = 0; i < json_object_array_length(array) && entry_count < max_entries; i++) {
        struct json_object *publication = json_object_array_get_idx(array, i);
        struct json_object *metadata = NULL;
        if (!json_object_object_get_ex(publication, "metadata", &metadata)) continue;
        const char *title = JsonString(metadata, "title");
        if (!title) continue;

        OPDSEntry *entry = &current_entries[entry_count++];
        memset(entry, 0, sizeof(*entry));
        strncpy(entry->title, title, sizeof(entry->title) - 1);
        const char *description = JsonString(metadata, "description");
        if (description) strncpy(entry->summary, description, sizeof(entry->summary) - 1);
        CopyAuthor(metadata, entry->author, sizeof(entry->author));
        entry->is_book = 1;

        struct json_object *images = NULL;
        if (json_object_object_get_ex(publication, "images", &images) &&
            json_object_is_type(images, json_type_array) &&
            json_object_array_length(images) > 0) {
            const char *image_url = JsonString(json_object_array_get_idx(images, 0), "href");
            if (image_url) {
                ResolveEbiblioURL(base_url, image_url, entry->cover_url, sizeof(entry->cover_url));
                strncpy(entry->thumb_url, entry->cover_url, sizeof(entry->thumb_url) - 1);
            }
        }
        CopyPublicationLinks(publication, base_url, entry);
        if (entry->ebiblio_status[0]) {
            const char *status = strcmp(entry->ebiblio_status, "available") == 0
                ? T("Disponible") : strcmp(entry->ebiblio_status, "unavailable") == 0
                ? T("No disponible") : entry->ebiblio_status;
            size_t used = strlen(entry->author);
            if (used < sizeof(entry->author) - 1) {
                snprintf(entry->author + used, sizeof(entry->author) - used,
                         "%s%s", used ? " | " : "", status);
            }
        }
        added++;
    }
    return added;
}

static void ReadFeedLinks(struct json_object *root, const char *base_url) {
    struct json_object *links = NULL;
    if (!json_object_object_get_ex(root, "links", &links) ||
        !json_object_is_type(links, json_type_array)) return;

    for (size_t i = 0; i < json_object_array_length(links); i++) {
        struct json_object *link = json_object_array_get_idx(links, i);
        struct json_object *rel = NULL;
        if (!json_object_object_get_ex(link, "rel", &rel)) continue;
        const char *href = JsonString(link, "href");
        if (!href) continue;
        if (RelContains(rel, "next")) {
            ResolveEbiblioURL(base_url, href, next_page_url, MAX_STR_LEN);
        } else if (RelContains(rel, "search")) {
            ResolveEbiblioURL(base_url, href, current_search_url, MAX_STR_LEN);
        }
    }
}

static int AddGroupEntries(struct json_object *groups, const char *base_url,
                           int max_entries) {
    if (!groups || !json_object_is_type(groups, json_type_array)) return 0;
    int added = 0;
    for (size_t i = 0; i < json_object_array_length(groups) && entry_count < max_entries; i++) {
        struct json_object *group = json_object_array_get_idx(groups, i);
        struct json_object *navigation = NULL;
        struct json_object *publications = NULL;
        if (json_object_object_get_ex(group, "navigation", &navigation)) {
            added += AddNavigationEntries(navigation, base_url, max_entries);
        }
        if (json_object_object_get_ex(group, "publications", &publications)) {
            added += AddPublicationEntries(publications, base_url, max_entries);
        }
    }
    return added;
}

int ParseEbiblioFeed(const char *json_data, const char *base_url) {
    if (!json_data || !base_url) return -1;

    entry_count = 0;
    next_page_url[0] = '\0';
    current_search_url[0] = '\0';
    current_feed_title[0] = '\0';
    total_results = 0;

    struct json_object *root = json_tokener_parse(json_data);
    if (!root || !json_object_is_type(root, json_type_object)) {
        if (root) json_object_put(root);
        return -1;
    }

    struct json_object *metadata = NULL;
    if (json_object_object_get_ex(root, "metadata", &metadata)) {
        const char *title = JsonString(metadata, "title");
        if (title) strncpy(current_feed_title, title, sizeof(current_feed_title) - 1);
        struct json_object *number = NULL;
        if (json_object_object_get_ex(metadata, "numberOfItems", &number) &&
            json_object_is_type(number, json_type_int)) {
            total_results = json_object_get_int(number);
        }
    }

    ReadFeedLinks(root, base_url);

    struct json_object *navigation = NULL;
    struct json_object *publications = NULL;
    int added = 0;
    if (json_object_object_get_ex(root, "navigation", &navigation)) {
        added += AddNavigationEntries(navigation, base_url, MAX_ENTRIES);
    }
    if (json_object_object_get_ex(root, "publications", &publications)) {
        added += AddPublicationEntries(publications, base_url, MAX_ENTRIES);
    }
    struct json_object *groups = NULL;
    if (json_object_object_get_ex(root, "groups", &groups)) {
        added += AddGroupEntries(groups, base_url, MAX_ENTRIES);
    }

    json_object_put(root);
    return 0;
}

int EbiblioLoadPublicationDetails(OPDSEntry *entry, const char *user, const char *pass,
                                  char *error, size_t error_size) {
    if (error && error_size) error[0] = '\0';
    if (!entry || !entry->ebiblio_self_url[0]) {
        if (error && error_size) {
            snprintf(error, error_size, T("La ficha de eBiblio no incluye un enlace de detalle."));
        }
        return -1;
    }

    struct MemoryStruct response = {NULL, 0};
    if (EbiblioFetchFeed(entry->ebiblio_self_url, user, pass, 1, &response,
                         error, error_size) != 0) {
        return -1;
    }

    struct json_object *root = json_tokener_parse(response.memory);
    free(response.memory);
    if (!root || !json_object_is_type(root, json_type_object)) {
        if (root) json_object_put(root);
        if (error && error_size) {
            snprintf(error, error_size, T("eBiblio devolvió una ficha de préstamo no válida."));
        }
        return -1;
    }

    struct json_object *publication = root;
    struct json_object *metadata = NULL;
    if (!json_object_object_get_ex(publication, "metadata", &metadata)) {
        struct json_object *publications = NULL;
        if (json_object_object_get_ex(root, "publications", &publications) &&
            json_object_is_type(publications, json_type_array)) {
            for (size_t i = 0; i < json_object_array_length(publications); i++) {
                struct json_object *candidate = json_object_array_get_idx(publications, i);
                struct json_object *candidate_metadata = NULL;
                if (json_object_object_get_ex(candidate, "metadata", &candidate_metadata) &&
                    strcmp(JsonString(candidate_metadata, "title")
                               ? JsonString(candidate_metadata, "title") : "",
                           entry->title) == 0) {
                    publication = candidate;
                    metadata = candidate_metadata;
                    break;
                }
            }
        }
    }

    if (!metadata || !json_object_is_type(metadata, json_type_object)) {
        json_object_put(root);
        if (error && error_size) {
            snprintf(error, error_size, T("La ficha de eBiblio no contiene metadatos de publicación."));
        }
        return -1;
    }

    CopyPublicationLinks(publication, entry->ebiblio_self_url, entry);
    entry->ebiblio_details_loaded = 1;
    json_object_put(root);
    return 0;
}

int FindEbiblioAccountURL(const char *json_data, const char *base_url,
                          char *account_url, size_t account_url_size) {
    if (!json_data || !base_url || !account_url || account_url_size == 0) return -1;
    account_url[0] = '\0';

    struct json_object *root = json_tokener_parse(json_data);
    if (!root || !json_object_is_type(root, json_type_object)) {
        if (root) json_object_put(root);
        return -1;
    }

    struct json_object *navigation = NULL;
    struct json_object *links = NULL;
    int found = 0;
    if (json_object_object_get_ex(root, "links", &links) &&
        json_object_is_type(links, json_type_array)) {
        for (size_t i = 0; i < json_object_array_length(links); i++) {
            struct json_object *link = json_object_array_get_idx(links, i);
            struct json_object *rel = NULL;
            const char *title = JsonString(link, "title");
            const char *href = JsonString(link, "href");
            if (!href) continue;

            if (title && (strstr(title, "Préstamos") || strstr(title, "préstamos"))) {
                ResolveEbiblioURL(base_url, href, account_url, account_url_size);
                found = account_url[0] != '\0';
            }
            if (!found && json_object_object_get_ex(link, "rel", &rel) &&
                RelContains(rel, "shelf")) {
                ResolveEbiblioURL(base_url, href, account_url, account_url_size);
                found = account_url[0] != '\0';
            }
            if (found) break;
        }
    }

    if (!found && json_object_object_get_ex(root, "navigation", &navigation) &&
        json_object_is_type(navigation, json_type_array)) {
        for (size_t i = 0; i < json_object_array_length(navigation); i++) {
            struct json_object *link = json_object_array_get_idx(navigation, i);
            const char *title = JsonString(link, "title");
            const char *href = JsonString(link, "href");
            if (!title || !href) continue;

            char lower_title[MAX_STR_LEN] = {0};
            strncpy(lower_title, title, sizeof(lower_title) - 1);
            for (size_t j = 0; lower_title[j]; j++) {
                lower_title[j] = (char)tolower((unsigned char)lower_title[j]);
            }
            if (strstr(lower_title, "préstam") || strstr(lower_title, "prestam") ||
                strstr(lower_title, "perfil") || strstr(lower_title, "activity")) {
                ResolveEbiblioURL(base_url, href, account_url, account_url_size);
                found = account_url[0] != '\0';
                if (found) break;
            }
        }
    }

    json_object_put(root);
    return found ? 0 : -1;
}
