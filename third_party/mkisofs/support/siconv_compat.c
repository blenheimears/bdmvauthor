/* Minimal libsiconv-compatible bridge for BDMV Author's private mkisofs. */
#include <schily/mconfig.h>
#include <stdio.h>
#include <schily/siconv.h>
#include <schily/stdio.h>
#include <schily/stdlib.h>
#include <schily/string.h>

#include <stdlib.h>
#include <string.h>
#ifdef USE_ICONV
#include <iconv.h>
#endif

static UInt8_t zero_page[256];
static siconvt_t *tables;

static char *
compat_strdup(const char *s)
{
    size_t n;
    char *p;
    if (s == NULL)
        return NULL;
    n = strlen(s) + 1;
    p = (char *)malloc(n);
    if (p != NULL)
        memcpy(p, s, n);
    return p;
}

static siconvt_t *
find_table(const char *name)
{
    siconvt_t *p;
    for (p = tables; p != NULL; p = p->sic_next) {
        if (strcmp(p->sic_name, name) == 0)
            return p;
    }
    return NULL;
}

static siconvt_t *
create_default(const char *name)
{
    siconvt_t *t = (siconvt_t *)calloc(1, sizeof(*t));
    UInt8_t *page0;
    unsigned i;
    if (t == NULL)
        return NULL;
    t->sic_name = compat_strdup(name);
    t->sic_cs2uni = (UInt16_t *)malloc(256 * sizeof(*t->sic_cs2uni));
    t->sic_uni2cs = (UInt8_t **)malloc(256 * sizeof(*t->sic_uni2cs));
    page0 = (UInt8_t *)malloc(256);
    if (t->sic_name == NULL || t->sic_cs2uni == NULL || t->sic_uni2cs == NULL || page0 == NULL) {
        free(page0); free(t->sic_uni2cs); free(t->sic_cs2uni); free(t->sic_name); free(t);
        return NULL;
    }
    for (i = 0; i < 256; ++i) {
        t->sic_cs2uni[i] = (UInt16_t)i;
        page0[i] = (UInt8_t)i;
        t->sic_uni2cs[i] = zero_page;
    }
    t->sic_uni2cs[0] = page0;
    t->sic_refcnt = 1;
    return t;
}

#ifdef USE_ICONV
static siconvt_t *
create_iconv_table(const char *display_name, const char *charset)
{
    siconvt_t *t = (siconvt_t *)calloc(1, sizeof(*t));
    iconv_t to_unicode;
    iconv_t from_unicode;
    if (t == NULL)
        return NULL;
    to_unicode = iconv_open("UCS-2BE", charset);
    if (to_unicode == (iconv_t)-1) {
        free(t);
        return NULL;
    }
    from_unicode = iconv_open(charset, "UCS-2BE");
    if (from_unicode == (iconv_t)-1) {
        iconv_close(to_unicode);
        free(t);
        return NULL;
    }
    t->sic_name = compat_strdup(display_name);
    if (t->sic_name == NULL) {
        iconv_close(to_unicode);
        iconv_close(from_unicode);
        free(t);
        return NULL;
    }
    t->sic_cd2uni = to_unicode;
    t->sic_uni2cd = from_unicode;
    t->sic_refcnt = 1;
    return t;
}
#endif

siconvt_t *
sic_open(char *name)
{
    siconvt_t *t;
    const char *charset;
    if (name == NULL || *name == '\0')
        name = (char *)"default";
    t = find_table(name);
    if (t != NULL) {
        ++t->sic_refcnt;
        return t;
    }
    if (strcmp(name, "default") == 0)
        t = create_default(name);
    else {
#ifdef USE_ICONV
        charset = strncmp(name, "iconv:", 6) == 0 ? name + 6 : name;
        t = create_iconv_table(name, charset);
#else
        (void)charset;
        t = NULL;
#endif
    }
    if (t != NULL) {
        t->sic_next = tables;
        tables = t;
    }
    return t;
}

const char *
sic_base(void)
{
    return NULL;
}

int
sic_list(FILE *stream)
{
    if (stream == NULL)
        stream = stderr;
    fprintf(stream, "default\n");
#ifdef USE_ICONV
    fprintf(stream, "Additional iconv-supported charset names may be used directly.\n");
#endif
    return 1;
}

int
sic_close(siconvt_t *t)
{
    siconvt_t **pp;
    if (t == NULL)
        return 0;
    if (--t->sic_refcnt > 0)
        return 0;
    for (pp = &tables; *pp != NULL; pp = &(*pp)->sic_next) {
        if (*pp == t) {
            *pp = t->sic_next;
            break;
        }
    }
#ifdef USE_ICONV
    if (t->sic_cd2uni != NULL)
        iconv_close(t->sic_cd2uni);
    if (t->sic_uni2cd != NULL)
        iconv_close(t->sic_uni2cd);
#endif
    if (t->sic_uni2cs != NULL) {
        if (t->sic_uni2cs[0] != zero_page)
            free(t->sic_uni2cs[0]);
        free(t->sic_uni2cs);
    }
    free(t->sic_cs2uni);
    free(t->sic_name);
    free(t);
    return 0;
}
