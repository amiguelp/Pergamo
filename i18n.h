#ifndef PERGAMO_I18N_H
#define PERGAMO_I18N_H

enum { LANG_AUTO = 0, LANG_ES = 1, LANG_EN = 2 };

extern int app_language_setting;

/* Devuelve la traducción del texto en español al idioma actual. */
const char *T(const char *es);
int CurrentLanguage(void);
const char *LanguageSettingName(int setting);
void LoadLanguageSetting(void);
void SetLanguageSetting(int setting);

#endif
