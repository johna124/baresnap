#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdarg.h>
#include <locale.h>

/*
 * txt2pdf.c v1.6
 *
 * Simple TXT to PDF converter in pure C.
 *
 * Features:
 * - Generates PDF 1.4 without external libraries.
 * - Uses Courier font.
 * - Creates PDF bookmarks (outline) for numbered sections.
 * - Creates internal links in the TOC.
 *
 * Changes v1.6:
 * - CRITICAL FIX: parse_section_line_dyn() now captures complete
 *   hierarchical IDs (2.3, 11.14, 13.1b, 26.5a, 2.0a, etc.).
 *   Previously only captured the first number ("2" for "2.3 ..."),
 *   which caused all TOC links of subsections to point to the
 *   main section.
 * - Support for subsections of any depth (2.3.1, 2.3.1.2).
 * - Support for alphanumeric suffixes (2.0a, 13.1b, 26.5a).
 * - Correct handling of "2. Title" (loose dot without subsection).
 *
 * Changes v1.5:
 * - Auto-detection of UTF-8 vs Latin-1/ISO-8859-1 encoding.
 * - Smart decoder in pdf_escape to translate UTF-8 to WinAnsiEncoding.
 * - Full support for accents, ñ and umlauts.
 *
 * Compile:
 *   gcc -O2 -Wall -Wextra -std=c11 -o txt2pdf txt2pdf.c
 *
 * Usage:
 *   ./txt2pdf readme.txt readme.pdf
 *   ./txt2pdf --debug readme.txt readme.pdf
 */

#define PAGE_WIDTH   595.0
#define PAGE_HEIGHT  842.0
#define MARGIN       50.0
#define FONT_SIZE    9.0
#define LEADING      12.0
#define CHAR_WIDTH   (FONT_SIZE * 0.6)

/* ========================================================================
 * 1. Header and Detection (UTF-8 vs Latin-1)
 * ======================================================================== */

static int g_is_utf8 = 0;

static int detect_if_utf8(const char *data, size_t len)
{
    int has_multibyte = 0;

    for (size_t i = 0; i < len; ) {
        unsigned char c = (unsigned char)data[i];

        if (c < 0x80) {
            i++;
        } else if ((c & 0xE0) == 0xC0) {
            if (i + 1 >= len || ((unsigned char)data[i + 1] & 0xC0) != 0x80)
                return 0;
            has_multibyte = 1;
            i += 2;
        } else if ((c & 0xF0) == 0xE0) {
            if (i + 2 >= len
                || ((unsigned char)data[i + 1] & 0xC0) != 0x80
                || ((unsigned char)data[i + 2] & 0xC0) != 0x80)
                return 0;
            has_multibyte = 1;
            i += 3;
        } else if ((c & 0xF8) == 0xF0) {
            if (i + 3 >= len
                || ((unsigned char)data[i + 1] & 0xC0) != 0x80
                || ((unsigned char)data[i + 2] & 0xC0) != 0x80
                || ((unsigned char)data[i + 3] & 0xC0) != 0x80)
                return 0;
            has_multibyte = 1;
            i += 4;
        } else {
            return 0;
        }
    }

    return has_multibyte;
}

/* ========================================================================
 * Memory structures and utilities
 * ======================================================================== */

typedef struct {
    char   *data;
    size_t  len;
    size_t  cap;
} Str;

static void *xmalloc(size_t n)
{
    void *p = malloc(n);
    if (!p) {
        fprintf(stderr, "Out of memory\n");
        exit(EXIT_FAILURE);
    }
    return p;
}

static void *xrealloc(void *p, size_t n)
{
    void *q = realloc(p, n);
    if (!q) {
        fprintf(stderr, "Out of memory\n");
        exit(EXIT_FAILURE);
    }
    return q;
}

static char *xstrdup(const char *s)
{
    size_t n = strlen(s) + 1;
    char  *p = xmalloc(n);
    memcpy(p, s, n);
    return p;
}

static char *xstrndup(const char *s, size_t n)
{
    char *p = xmalloc(n + 1);
    if (n > 0)
        memcpy(p, s, n);
    p[n] = '\0';
    return p;
}

static void str_reserve(Str *s, size_t need)
{
    if (need == 0)
        need = 1;
    if (s->cap < need) {
        size_t ncap = s->cap ? s->cap : 64;
        while (ncap < need)
            ncap *= 2;
        s->data = xrealloc(s->data, ncap);
        s->cap  = ncap;
    }
    if (s->data && s->len < s->cap)
        s->data[s->len] = '\0';
}

static void str_init(Str *s)
{
    s->data = NULL;
    s->len  = 0;
    s->cap  = 0;
    str_reserve(s, 1);
    s->len = 0;
    s->data[0] = '\0';
}

static void str_free(Str *s)
{
    free(s->data);
    s->data = NULL;
    s->len  = 0;
    s->cap  = 0;
}

static void str_append(Str *s, const char *data, size_t len)
{
    str_reserve(s, s->len + len + 1);
    memcpy(s->data + s->len, data, len);
    s->len += len;
    s->data[s->len] = '\0';
}

static void str_append_str(Str *s, const char *str)
{
    str_append(s, str, strlen(str));
}

static void str_append_char(Str *s, char c)
{
    str_reserve(s, s->len + 2);
    s->data[s->len++] = c;
    s->data[s->len]   = '\0';
}

static void str_appendf(Str *s, const char *fmt, ...)
{
    va_list ap, ap2;
    va_start(ap, fmt);
    va_copy(ap2, ap);

    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);

    if (n < 0) {
        va_end(ap2);
        return;
    }

    str_reserve(s, s->len + (size_t)n + 1);
    vsnprintf(s->data + s->len, (size_t)n + 1, fmt, ap2);
    s->len += (size_t)n;
    va_end(ap2);
}

/* ========================================================================
 * 2. 'pdf_escape' function (Smart Decoder)
 * ======================================================================== */

static void pdf_escape(const char *s, Str *out)
{
    if (!out->data) {
        out->len = 0;
        out->cap = 0;
        str_init(out);
    } else {
        out->len = 0;
        if (out->cap > 0)
            out->data[0] = '\0';
    }

    const unsigned char *p = (const unsigned char *)s;

    while (*p) {
        unsigned char c = *p;

        /* Dynamic UTF-8 -> WinAnsiEncoding translation */
        if (g_is_utf8 && c >= 0xC2 && c <= 0xDF) {
            unsigned char c2 = p[1];
            if (c2 >= 0x80 && c2 <= 0xBF) {
                int cp = ((c & 0x1F) << 6) | (c2 & 0x3F);
                unsigned char winansi = 0;

                switch (cp) {
                    case 0x00E1: winansi = 0xE1; break; /* á */
                    case 0x00E9: winansi = 0xE9; break; /* é */
                    case 0x00ED: winansi = 0xED; break; /* í */
                    case 0x00F3: winansi = 0xF3; break; /* ó */
                    case 0x00FA: winansi = 0xFA; break; /* ú */
                    case 0x00F1: winansi = 0xF1; break; /* ñ */
                    case 0x00FC: winansi = 0xFC; break; /* ü */
                    case 0x00C1: winansi = 0xC1; break; /* Á */
                    case 0x00C9: winansi = 0xC9; break; /* É */
                    case 0x00CD: winansi = 0xCD; break; /* Í */
                    case 0x00D3: winansi = 0xD3; break; /* Ó */
                    case 0x00DA: winansi = 0xDA; break; /* Ú */
                    case 0x00D1: winansi = 0xD1; break; /* Ñ */
                    case 0x00DC: winansi = 0xDC; break; /* Ü */
                    case 0x00BF: winansi = 0xBF; break; /* ¿ */
                    case 0x00A1: winansi = 0xA1; break; /* ¡ */
                    default:     winansi = '?';  break;
                }

                str_append_char(out, (char)winansi);
                p += 2;
                continue;
            }
        }

        if      (c == '\\') str_append_str(out, "\\\\");
        else if (c == '(')  str_append_str(out, "\\(");
        else if (c == ')')  str_append_str(out, "\\)");
        else if (c == '\r') str_append_str(out, "\\r");
        else if (c == '\n') str_append_str(out, "\n");
        else if (c >= 32)   str_append_char(out, (char)c);
        else if (c == '\t') str_append_char(out, ' ');
        else                str_append_char(out, '?');

        p++;
    }
}

/* ========================================================================
 * Text and calculation utilities
 * ======================================================================== */

static double visible_width(const char *s)
{
    size_t n = strlen(s);
    while (n > 0 && isspace((unsigned char)s[n - 1]))
        n--;
    return (double)n * CHAR_WIDTH;
}

static void next_visual_chunk(const char *s,
                              size_t      off,
                              size_t      slen,
                              int         max_chars,
                              char      **out_text,
                              size_t     *out_consume)
{
    size_t remaining = slen - off;

    if (remaining <= (size_t)max_chars) {
        *out_text    = xstrndup(s + off, remaining);
        *out_consume = remaining;
        return;
    }

    int br = -1;
    for (int k = max_chars - 1; k >= 0; --k) {
        if (isspace((unsigned char)s[off + (size_t)k])) {
            br = k;
            break;
        }
    }

    if (br < 0) {
        for (int k = max_chars - 1; k >= 0; --k) {
            unsigned char c = (unsigned char)s[off + (size_t)k];
            if (c == '-' || c == '/') {
                br = k;
                break;
            }
        }
    }

    if (br >= 0) {
        size_t len = (size_t)br + 1;
        *out_text    = xstrndup(s + off, len);
        *out_consume = len;
        return;
    }

    if (off + (size_t)max_chars < slen
        && isspace((unsigned char)s[off + (size_t)max_chars])) {
        *out_text    = xstrndup(s + off, (size_t)max_chars);
        *out_consume = (size_t)max_chars;
        return;
    }

    if (max_chars > 1) {
        size_t len = (size_t)max_chars - 1;
        char  *buf = xmalloc(len + 2);
        memcpy(buf, s + off, len);
        buf[len]     = '-';
        buf[len + 1] = '\0';
        *out_text    = buf;
        *out_consume = len;
    } else {
        *out_text    = xstrndup(s + off, 1);
        *out_consume = 1;
    }
}

static char *read_file(const char *path, size_t *out_len)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;

    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }

    long sz = ftell(f);
    if (sz < 0) { fclose(f); return NULL; }

    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return NULL; }

    char  *buf = xmalloc((size_t)sz + 1);
    size_t rd  = fread(buf, 1, (size_t)sz, f);
    buf[rd] = '\0';
    fclose(f);

    if (out_len)
        *out_len = rd;

    return buf;
}

static char **split_lines(const char *data, size_t len, int *out_n)
{
    char  **lines = NULL;
    size_t  n = 0, cap = 0;

    if (len == 0) {
        lines = xmalloc(sizeof(char *));
        lines[0] = xstrdup("");
        n = 1;
    } else {
        const char *p   = data;
        const char *end = data + len;

        while (p < end) {
            const char *start = p;
            while (p < end && *p != '\n')
                p++;

            size_t l = (size_t)(p - start);
            if (l > 0 && start[l - 1] == '\r')
                l--;

            char *line = xmalloc(l + 1);
            memcpy(line, start, l);
            line[l] = '\0';

            if (n == cap) {
                cap   = cap ? cap * 2 : 64;
                lines = xrealloc(lines, cap * sizeof(char *));
            }
            lines[n++] = line;

            if (p < end && *p == '\n')
                p++;
        }
    }

    *out_n = (int)n;
    return lines;
}

static char *trim_dup(const char *src)
{
    const char *p = src;
    while (isspace((unsigned char)*p))
        p++;

    size_t len = strlen(p);
    while (len > 0 && isspace((unsigned char)p[len - 1]))
        len--;

    return xstrndup(p, len);
}

static int is_separator(const char *line)
{
    int count = 0;
    const char *p = line;

    while (*p) {
        if (*p == '=')
            count++;
        else if (!isspace((unsigned char)*p))
            return 0;
        p++;
    }

    return count >= 10;
}

static int line_is_blank(const char *s)
{
    while (*s) {
        if (!isspace((unsigned char)*s))
            return 0;
        s++;
    }
    return 1;
}

static int nearby_prev_is_separator(char **lines, int nlines, int i)
{
    (void)nlines;
    for (int j = i - 1; j >= 0 && j >= i - 5; --j) {
        if (line_is_blank(lines[j]))
            continue;
        return is_separator(lines[j]);
    }
    return 0;
}

static int nearby_next_is_separator(char **lines, int nlines, int i)
{
    for (int j = i + 1; j < nlines && j <= i + 5; ++j) {
        if (line_is_blank(lines[j]))
            continue;
        return is_separator(lines[j]);
    }
    return 0;
}

/* ========================================================================
 * SECTION PARSER — v1.6: COMPLETE HIERARCHICAL IDs
 * ========================================================================
 *
 * Correctly captures:
 *   "1. Title"                     -> id = "1"
 *   "2.3 Authentication SSH"       -> id = "2.3"
 *   "2.0a Logical order"           -> id = "2.0a"
 *   "11.14 Summary"                -> id = "11.14"
 *   "13.1b Variables"              -> id = "13.1b"
 *   "26.5a Validation"             -> id = "26.5a"
 *   "2.3.1 Deep subsection"        -> id = "2.3.1"
 *
 * BUG v1.5: the previous parser only captured the first number,
 * so "2.3 Authentication" and "2. ARCHITECTURE" both produced
 * id = "2", causing TOC links of subsections to point to the
 * main section.
 */

static int parse_section_line_dyn(const char *line,
                                  char      **id_out,
                                  char      **title_out)
{
    const char *p = line;

    while (isspace((unsigned char)*p))
        p++;

    if (!isdigit((unsigned char)*p))
        return 0;

    Str id;
    str_init(&id);

    /* Read first number */
    while (isdigit((unsigned char)*p)) {
        str_append_char(&id, *p);
        p++;
    }

    /* Allow letters attached to the first number (e.g. "2a", "13b") */
    while (isalpha((unsigned char)*p)) {
        str_append_char(&id, (char)tolower((unsigned char)*p));
        p++;
    }

    /* Read hierarchical subsections: ".N", ".Na", ".N.M", etc. */
    while (*p == '.') {
        const char *save_p = p;
        p++;  /* skip the dot tentatively */

        /* Must have at least one digit after the dot to be a valid
         * subsection. Otherwise, rollback (it was a loose dot). */
        if (!isdigit((unsigned char)*p)) {
            p = save_p;
            break;
        }

        str_append_char(&id, '.');

        while (isdigit((unsigned char)*p)) {
            str_append_char(&id, *p);
            p++;
        }

        /* Allow letters at the end (e.g. "2.0a", "13.1b", "26.5a") */
        while (isalpha((unsigned char)*p)) {
            str_append_char(&id, (char)tolower((unsigned char)*p));
            p++;
        }
    }

    /* Consume a loose "." if remaining (e.g. "2. Title" without subsection) */
    if (*p == '.')
        p++;

    /* Must have a space or end of line */
    if (*p != '\0' && !isspace((unsigned char)*p)) {
        str_free(&id);
        return 0;
    }

    while (isspace((unsigned char)*p))
        p++;

    char *title = trim_dup(p);

    if (id.len == 0) {
        str_free(&id);
        free(title);
        return 0;
    }

    *id_out    = id.data;
    *title_out = title;
    return 1;
}

static int is_upper_title(const char *title)
{
    int has = 0;
    for (const char *p = title; *p; ++p) {
        if (isalpha((unsigned char)*p)) {
            has = 1;
            if (!isupper((unsigned char)*p))
                return 0;
        }
    }
    return has;
}

static char *normalize_alnum_upper_dup(const char *s)
{
    Str out;
    str_init(&out);

    for (const char *p = s; *p; ++p) {
        unsigned char c = (unsigned char)*p;
        if (isalnum(c))
            str_append_char(&out, (char)toupper(c));
    }

    return out.data;
}

typedef struct {
    char   *id;
    char   *title;
    char   *full;
    char   *title_norm;
    int     orig_line;
    int     page;
    double  y;
} Section;

typedef struct {
    int     orig_line;
    int     target;
    int     page;
    double  y;
    double  width;
    char   *text;
} Candidate;

typedef struct {
    int     page;
    int     orig_line;
    double  y;
    char   *text;
} VisLine;

static int find_section_by_id(Section *secs, size_t n, const char *id)
{
    if (!id || !id[0])
        return -1;
    for (size_t i = 0; i < n; i++) {
        if (strcmp(secs[i].id, id) == 0)
            return (int)i;
    }
    return -1;
}

static int find_section_by_title_norm(Section *secs, size_t n, const char *norm)
{
    if (!norm || !norm[0])
        return -1;
    for (size_t i = 0; i < n; i++) {
        if (strcmp(secs[i].title_norm, norm) == 0)
            return (int)i;
    }
    return -1;
}

static int add_section(Section **secs, size_t *n, size_t *cap,
                       const char *id, const char *title,
                       const char *full, int orig_line)
{
    if (*n == *cap) {
        *cap  = *cap ? *cap * 2 : 32;
        *secs = xrealloc(*secs, *cap * sizeof(Section));
    }

    Section *s = &(*secs)[*n];
    memset(s, 0, sizeof(*s));

    s->id         = xstrdup(id    ? id    : "");
    s->title      = xstrdup(title ? title : "");
    s->full       = xstrdup(full  ? full  : "");
    s->title_norm = normalize_alnum_upper_dup(s->title);
    s->orig_line  = orig_line;
    s->page       = 0;
    s->y          = 0.0;

    return (int)(*n)++;
}

static int add_candidate(Candidate **cands, size_t *n, size_t *cap,
                         int orig_line, int target)
{
    if (target < 0)
        return -1;

    if (*n == *cap) {
        *cap  = *cap ? *cap * 2 : 32;
        *cands = xrealloc(*cands, *cap * sizeof(Candidate));
    }

    Candidate *c = &(*cands)[*n];
    c->orig_line = orig_line;
    c->target    = target;
    c->page      = 0;
    c->y         = 0.0;
    c->width     = 0.0;
    c->text      = NULL;

    return (int)(*n)++;
}

/* ========================================================================
 * 3. Integration in 'main()'
 * ======================================================================== */

int main(int argc, char **argv)
{
    setlocale(LC_NUMERIC, "C");

    int         debug  = 0;
    const char *input  = NULL;
    const char *output = NULL;

    if (argc == 2
        && (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0)) {
        printf("Usage: %s [--debug] input.txt output.pdf\n", argv[0]);
        return 0;
    }

    if (argc == 4 && strcmp(argv[1], "--debug") == 0) {
        debug  = 1;
        input  = argv[2];
        output = argv[3];
    } else if (argc == 3) {
        input  = argv[1];
        output = argv[2];
    } else {
        fprintf(stderr, "Usage: %s [--debug] input.txt output.pdf\n", argv[0]);
        return 1;
    }

    size_t  len  = 0;
    char   *data = read_file(input, &len);

    if (!data) {
        fprintf(stderr, "Cannot read '%s'\n", input);
        return 1;
    }

    g_is_utf8 = detect_if_utf8(data, len);

    if (debug) {
        fprintf(stderr, "[debug] Detected encoding: %s\n",
                g_is_utf8 ? "UTF-8" : "Latin-1/ISO-8859-1");
    }

    int     nlines = 0;
    char  **lines  = split_lines(data, len, &nlines);
    free(data);
    data = NULL;

    int map_n = nlines > 0 ? nlines : 1;

    int *heading_map   = xmalloc(sizeof(int) * (size_t)map_n);
    int *candidate_map = xmalloc(sizeof(int) * (size_t)map_n);
    int *toc_map       = xmalloc(sizeof(int) * (size_t)map_n);

    for (int i = 0; i < map_n; i++) {
        heading_map[i]   = -1;
        candidate_map[i] = -1;
        toc_map[i]       = 0;
    }

    Section   *sections     = NULL;
    size_t     nsections    = 0, capsections = 0;
    Candidate *cands        = NULL;
    size_t     ncands       = 0, capcands = 0;
    VisLine   *vis          = NULL;
    size_t     nvis         = 0, capvis = 0;

    int    *outline_idx = NULL;
    long   *offsets     = NULL;
    FILE   *out         = NULL;
    int     rc          = 0;
    int     toc_line_count = 0;

    Str esc;
    esc.data = NULL;
    esc.len  = 0;
    esc.cap  = 0;

    /* ---- Detect TOC block to avoid taking its lines as sections ---- */
    for (int i = 0; i < nlines; i++) {
        if (toc_map[i])
            continue;

        char *trimmed = trim_dup(lines[i]);
        int is_toc_title = (strstr(trimmed, "[TOC]") != NULL);
        free(trimmed);

        if (!is_toc_title)
            continue;

        int j = i + 1;
        while (j < nlines && line_is_blank(lines[j]))
            j++;

        if (j < nlines && is_separator(lines[j])) {
            int k = j + 1;
            while (k < nlines && !is_separator(lines[k]))
                k++;

            int end = (k < nlines) ? k : (nlines - 1);

            for (int x = i; x <= end; x++) {
                toc_map[x] = 1;
                toc_line_count++;
            }
            i = end;
        }
    }

    /* ---- Detect sections in the body ---- */
    for (int i = 0; i < nlines; i++) {
        char *id = NULL, *title = NULL;

        if (!parse_section_line_dyn(lines[i], &id, &title))
            continue;

        int valid = is_upper_title(title);

        if (valid && toc_map[i]) {
            if (debug)
                fprintf(stderr, "[debug] SKIP line %d: heading inside TOC\n",
                        i + 1);
            valid = 0;
        }

        if (valid
            && (!nearby_prev_is_separator(lines, nlines, i)
                || !nearby_next_is_separator(lines, nlines, i))) {
            if (debug)
                fprintf(stderr,
                        "[debug] SKIP line %d: heading not between separators\n",
                        i + 1);
            valid = 0;
        }

        if (valid) {
            char *full = trim_dup(lines[i]);
            int idx = add_section(&sections, &nsections, &capsections,
                                  id, title, full, i);
            heading_map[i] = idx;
            free(full);
        }

        free(id);
        free(title);
    }

    /* ---- Detect links in the TOC ---- */
    for (int i = 0; i < nlines; i++) {
        if (!toc_map[i])
            continue;
        if (candidate_map[i] != -1)
            continue;

        char *id = NULL, *title = NULL;
        if (!parse_section_line_dyn(lines[i], &id, &title))
            continue;

        int target = find_section_by_id(sections, nsections, id);
        if (target < 0) {
            char *norm = normalize_alnum_upper_dup(title);
            target = find_section_by_title_norm(sections, nsections, norm);
            free(norm);
        }

        if (target >= 0) {
            int idx = add_candidate(&cands, &ncands, &capcands, i, target);
            if (idx >= 0)
                candidate_map[i] = idx;
        }

        free(id);
        free(title);
    }

    /* ---- Page layout ---- */
    int current_page   = 1;
    int line_on_page   = 0;
    int max_chars      = (int)((PAGE_WIDTH - 2.0 * MARGIN) / CHAR_WIDTH);
    if (max_chars < 1)
        max_chars = 1;

    int lines_per_page = (int)((PAGE_HEIGHT - 2.0 * MARGIN) / LEADING);
    if (lines_per_page < 1)
        lines_per_page = 1;

    for (int i = 0; i < nlines; i++) {
        const char *s = lines[i];
        size_t      slen = strlen(s);
        size_t      off  = 0;
        int         first = 1, emitted = 0;

        while (off < slen || !emitted) {
            char  *buf     = NULL;
            size_t consume = 0;

            if (off >= slen) {
                buf     = xstrdup("");
                consume = 0;
            } else {
                next_visual_chunk(s, off, slen, max_chars, &buf, &consume);
            }

            if (line_on_page >= lines_per_page) {
                current_page++;
                line_on_page = 0;
            }

            double y = PAGE_HEIGHT - MARGIN - (double)line_on_page * LEADING;

            if (nvis == capvis) {
                capvis = capvis ? capvis * 2 : 256;
                vis    = xrealloc(vis, capvis * sizeof(VisLine));
            }

            vis[nvis].page      = current_page;
            vis[nvis].orig_line = i;
            vis[nvis].y         = y;
            vis[nvis].text      = buf;
            nvis++;

            if (first) {
                if (heading_map[i] >= 0) {
                    sections[heading_map[i]].page = current_page;
                    sections[heading_map[i]].y    = y;
                }

                if (candidate_map[i] >= 0) {
                    Candidate *c = &cands[candidate_map[i]];
                    c->page  = current_page;
                    c->y     = y;
                    free(c->text);
                    c->text  = xstrdup(buf);
                    c->width = visible_width(buf);
                }
                first = 0;
            }

            off += consume;
            emitted = 1;
            line_on_page++;
        }
    }

    int total_pages = current_page;
    if (total_pages < 1)
        total_pages = 1;

    /* ---- Outline (PDF bookmarks) ---- */
    size_t outline_count = 0;

    if (nsections > 0) {
        outline_idx = xmalloc(sizeof(int) * nsections);
        for (size_t i = 0; i < nsections; i++) {
            if (sections[i].page > 0)
                outline_idx[outline_count++] = (int)i;
        }
    }

    int K = (int)outline_count;

    if (debug) {
        fprintf(stderr, "[debug] Lines marked as TOC: %d\n", toc_line_count);
        fprintf(stderr, "[debug] Sections detected: %zu\n", nsections);

        for (size_t i = 0; i < nsections; i++) {
            Section *s = &sections[i];
            fprintf(stderr,
                    "[debug] SECTION line %d id '%s' page %d y %.2f title '%s'\n",
                    s->orig_line + 1, s->id, s->page, s->y, s->title);
        }

        fprintf(stderr, "[debug] TOC links detected: %zu\n", ncands);

        for (size_t i = 0; i < ncands; i++) {
            Candidate *c = &cands[i];
            if (c->target >= 0 && (size_t)c->target < nsections) {
                Section *s = &sections[c->target];
                fprintf(stderr,
                        "[debug] LINK line %d id '%s' -> section '%s' page %d\n",
                        c->orig_line + 1,
                        sections[c->target].id,
                        s->title,
                        s->page);
            }
        }
    }

    /* ---- Generate PDF ---- */
    out = fopen(output, "wb");
    if (!out) {
        fprintf(stderr, "Cannot write '%s'\n", output);
        rc = 1;
        goto cleanup;
    }

    int first_page_obj    = 5;
    int first_content_obj = first_page_obj + total_pages;
    int first_outline_obj = first_content_obj + total_pages;
    int total_objs        = 4 + 2 * total_pages + K;

    offsets = xmalloc(sizeof(long) * (size_t)(total_objs + 1));

    fprintf(out, "%%PDF-1.4\n");

    static const unsigned char bincomment[] = {
        '%', 0xE2, 0xE3, 0xCF, 0xD3, '\n'
    };
    fwrite(bincomment, 1, sizeof(bincomment), out);

    /* Catalog */
    offsets[1] = ftell(out);
    if (K > 0) {
        fprintf(out,
                "1 0 obj\n"
                "<< /Type /Catalog /Pages 3 0 R /Outlines 2 0 R /PageMode /UseOutlines >>\n"
                "endobj\n");
    } else {
        fprintf(out,
                "1 0 obj\n"
                "<< /Type /Catalog /Pages 3 0 R >>\n"
                "endobj\n");
    }

    /* Outlines root */
    offsets[2] = ftell(out);
    if (K > 0) {
        fprintf(out,
                "2 0 obj\n"
                "<< /Type /Outlines /First %d 0 R /Last %d 0 R /Count %d >>\n"
                "endobj\n",
                first_outline_obj,
                first_outline_obj + K - 1,
                K);
    } else {
        fprintf(out,
                "2 0 obj\n"
                "<< /Type /Outlines /Count 0 >>\n"
                "endobj\n");
    }

    /* Pages */
    offsets[3] = ftell(out);

    Str kids;
    str_init(&kids);
    str_append_str(&kids, "[");

    for (int p = 1; p <= total_pages; p++) {
        if (p > 1)
            str_append_char(&kids, ' ');
        str_appendf(&kids, "%d 0 R", first_page_obj + p - 1);
    }
    str_append_str(&kids, "]");

    fprintf(out,
            "3 0 obj\n"
            "<< /Type /Pages /Kids %s /Count %d >>\n"
            "endobj\n",
            kids.data, total_pages);
    str_free(&kids);

    /* Font */
    offsets[4] = ftell(out);
    fprintf(out,
            "4 0 obj\n"
            "<< /Type /Font /Subtype /Type1 /BaseFont /Courier /Encoding /WinAnsiEncoding >>\n"
            "endobj\n");

    str_init(&esc);

    /* Pages */
    for (int p = 1; p <= total_pages; p++) {
        int page_obj    = first_page_obj + p - 1;
        int content_obj = first_content_obj + p - 1;

        Str annots;
        str_init(&annots);
        str_append_str(&annots, "[");

        int has_annots = 0;

        for (size_t ci = 0; ci < ncands; ci++) {
            Candidate *c = &cands[ci];

            if (c->page == p
                && c->target >= 0
                && c->text
                && c->width > 1.0) {

                Section *s = &sections[c->target];

                if (s->page > 0) {
                    int target_page_obj = first_page_obj + s->page - 1;
                    double dest_y = s->y + 10.0;

                    if (dest_y > PAGE_HEIGHT - MARGIN)
                        dest_y = PAGE_HEIGHT - MARGIN;

                    double x1 = MARGIN - 1.0;
                    double y1 = c->y - 2.0;
                    double x2 = MARGIN + c->width + 1.0;
                    double y2 = c->y + FONT_SIZE;

                    if (has_annots)
                        str_append_char(&annots, ' ');

                    str_appendf(&annots,
                                "<< /Type /Annot /Subtype /Link "
                                "/Rect [%.2f %.2f %.2f %.2f] "
                                "/Border [0 0 0] "
                                "/Dest [%d 0 R /XYZ %.2f %.2f null] >>",
                                x1, y1, x2, y2,
                                target_page_obj,
                                MARGIN, dest_y);
                    has_annots = 1;
                }
            }
        }

        str_append_str(&annots, "]");

        offsets[page_obj] = ftell(out);
        fprintf(out,
                "%d 0 obj\n"
                "<< /Type /Page /Parent 3 0 R "
                "/MediaBox [0 0 %.0f %.0f] "
                "/Resources << /Font << /F1 4 0 R >> >> "
                "/Contents %d 0 R",
                page_obj, PAGE_WIDTH, PAGE_HEIGHT, content_obj);

        if (has_annots)
            fprintf(out, " /Annots %s", annots.data);

        fprintf(out, " >>\nendobj\n");
        str_free(&annots);
    }

    /* Content streams */
    for (int p = 1; p <= total_pages; p++) {
        int content_obj = first_content_obj + p - 1;

        Str content;
        str_init(&content);

        for (size_t vi = 0; vi < nvis; vi++) {
            if (vis[vi].page == p
                && vis[vi].text
                && vis[vi].text[0]) {

                pdf_escape(vis[vi].text, &esc);

                str_appendf(&content,
                            "BT /F1 %.1f Tf %.2f %.2f Td (%s) Tj ET\n",
                            FONT_SIZE, MARGIN, vis[vi].y,
                            esc.data ? esc.data : "");
            }
        }

        offsets[content_obj] = ftell(out);
        fprintf(out,
                "%d 0 obj\n"
                "<< /Length %zu >>\n"
                "stream\n",
                content_obj, content.len);

        if (content.len > 0)
            fwrite(content.data, 1, content.len, out);

        fprintf(out, "endstream\nendobj\n");
        str_free(&content);
    }

    /* Outline items (bookmarks) */
    for (int j = 0; j < K; j++) {
        int      si       = outline_idx[j];
        Section *s        = &sections[si];
        int      obj      = first_outline_obj + j;
        int      page_obj = first_page_obj + s->page - 1;
        double   dest_y   = s->y + 10.0;

        if (dest_y > PAGE_HEIGHT - MARGIN)
            dest_y = PAGE_HEIGHT - MARGIN;

        pdf_escape(s->full, &esc);

        offsets[obj] = ftell(out);
        fprintf(out,
                "%d 0 obj\n"
                "<< /Title (%s) /Parent 2 0 R",
                obj, esc.data ? esc.data : "");

        if (j > 0)
            fprintf(out, " /Prev %d 0 R", obj - 1);
        if (j < K - 1)
            fprintf(out, " /Next %d 0 R", obj + 1);

        fprintf(out,
                " /Dest [%d 0 R /XYZ %.2f %.2f null] >>\n"
                "endobj\n",
                page_obj, MARGIN, dest_y);
    }

    /* Xref + trailer */
    long xref_pos = ftell(out);
    fprintf(out, "xref\n0 %d\n", total_objs + 1);
    fprintf(out, "0000000000 65535 f \n");

    for (int i = 1; i <= total_objs; i++)
        fprintf(out, "%010ld 00000 n \n", offsets[i]);

    fprintf(out,
            "trailer\n"
            "<< /Size %d /Root 1 0 R >>\n"
            "startxref\n"
            "%ld\n",
            total_objs + 1, xref_pos);

    fwrite("%%EOF\n", 1, sizeof("%%EOF\n") - 1, out);

    printf("PDF generated: %s\n", output);
    printf("Pages: %d | Bookmarks: %d | Internal links: %zu\n",
           total_pages, K, ncands);

    rc = 0;

cleanup:
    if (out)
        fclose(out);

    if (lines) {
        for (int i = 0; i < nlines; i++)
            free(lines[i]);
        free(lines);
    }

    free(heading_map);
    free(candidate_map);
    free(toc_map);

    if (sections) {
        for (size_t i = 0; i < nsections; i++) {
            free(sections[i].id);
            free(sections[i].title);
            free(sections[i].full);
            free(sections[i].title_norm);
        }
        free(sections);
    }

    if (cands) {
        for (size_t i = 0; i < ncands; i++)
            free(cands[i].text);
        free(cands);
    }

    if (vis) {
        for (size_t i = 0; i < nvis; i++)
            free(vis[i].text);
        free(vis);
    }

    free(outline_idx);
    free(offsets);
    str_free(&esc);

    return rc;
}
