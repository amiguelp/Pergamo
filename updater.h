#ifndef PERGAMO_UPDATER_H
#define PERGAMO_UPDATER_H

#include <stddef.h>

typedef void (*UpdateProgressFn)(int percent);

/* -1 si a < b, 0 si iguales, 1 si a > b ("1.10" > "1.9"). */
int CompareVersions(const char *a, const char *b);

/* 1 si hay una versión más nueva publicada, 0 si no, -1 si error. */
int UpdateCheck(char *remote_version, size_t remote_size, char *error, size_t error_size);

/* Descarga, verifica e instala la versión publicada. 0 si todo fue bien. */
int UpdateInstall(UpdateProgressFn progress, char *error, size_t error_size);

/* Borra la copia de la versión anterior tras arrancar bien la nueva. */
void UpdateCleanupPreviousVersion(void);

#endif
