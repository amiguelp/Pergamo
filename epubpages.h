#ifndef PERGAMO_EPUBPAGES_H
#define PERGAMO_EPUBPAGES_H

/* Caracteres (con espacios) de una "página estándar". */
#define EPUB_CHARS_PER_PAGE 1800

typedef struct {
    int print_pages;      /* páginas de la edición impresa según el EPUB (0 = no trae) */
    long text_chars;      /* caracteres del texto */
    int estimated_pages;  /* text_chars / EPUB_CHARS_PER_PAGE */
    char isbn[16];        /* ISBN de los metadatos, si lo hay */
} EpubPageInfo;

/* 0 si se pudo leer el EPUB. */
int EpubAnalyze(const char *path, EpubPageInfo *info);

#endif
