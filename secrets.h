#ifndef PERGAMO_SECRETS_H
#define PERGAMO_SECRETS_H

#include <stddef.h>

enum {
    SECRET_OK = 0,          /* descifrado (o vacío) */
    SECRET_PLAINTEXT = 1,   /* estaba en texto plano: hay que volver a guardarlo */
    SECRET_LOCKED = 2,      /* protegido con PIN y todavía no se ha escrito */
    SECRET_ERROR = -1       /* no se puede descifrar (otro lector, archivo dañado...) */
};

/* Parámetros guardados en la configuración (sal, PIN activo, verificador). */
void SecretsLoadParams(const char *salt_b64, int pin_flag, const char *check);
void SecretsGetParams(char *salt_b64, size_t salt_size, int *pin_flag,
                      char *check, size_t check_size);

int SecretsPinEnabled(void);
int SecretsPinUnlocked(void);
int SecretsIsEncrypted(const char *stored);
int SecretsNeedsPin(const char *stored);

int SecretsDecrypt(const char *stored, char *out, size_t out_size);
int SecretsEncrypt(const char *plain, char *out, size_t out_size);

int SecretsUnlock(const char *pin);       /* 0 si el PIN es correcto */
int SecretsEnablePin(const char *pin);    /* las contraseñas deben estar descifradas */
void SecretsDisablePin(void);             /* idem */

#endif
