#include <assert.h>
#include <stdio.h>
#include <float.h>
#include <string.h>

#ifdef WIN32
#include <windows.h>
#include <io.h>
#include <fileapi.h>
#endif

#include "jv.h"
#include "jv_dtoa.h"
#include "jv_unicode.h"

#define ESC "\033"
#define COL(c) (ESC "[" c "m")
#define COLRESET (ESC "[0m")

// Colour table. See http://en.wikipedia.org/wiki/ANSI_escape_code#Colors
// for how to choose these.
static const jv_kind colour_kinds[] = 
  {JV_KIND_NULL,   JV_KIND_FALSE, JV_KIND_TRUE, JV_KIND_NUMBER,
   JV_KIND_STRING, JV_KIND_ARRAY, JV_KIND_OBJECT};
static const char* const colours[] =
  {COL("1;30"),    COL("0;39"),      COL("0;39"),     COL("0;39"),
   COL("0;32"),      COL("1;39"),     COL("1;39")};
#define FIELD_COLOUR COL("34;1")

/*
 * The Windows CRT and console are something else.  In order for the
 * console to get UTF-8 written to it correctly we have to bypass stdio
 * completely.  No amount of fflush()ing helps.  If the first byte of a
 * buffer being written with fwrite() is non-ASCII UTF-8 then the
 * console misinterprets the byte sequence.  But one must not
 * WriteFile() if stdout is a file!1!!
 *
 * We carry knowledge of whether the FILE * is a tty everywhere we
 * output to it just so we can write with WriteFile() if stdout is a
 * console on WIN32.
 */

void priv_fwrite(const char *s, size_t len, FILE *fout, int is_tty) {
#ifdef WIN32
  if (is_tty)
    WriteFile((HANDLE)_get_osfhandle(fileno(fout)), s, len, NULL, NULL);
  else
    fwrite(s, 1, len, fout);
#else
  fwrite(s, 1, len, fout);
#endif
}

static void put_buf(const char *s, int len, FILE *fout, jv *strout, int is_tty) {
  if (strout) {
    *strout = jv_string_append_buf(*strout, s, len);
  } else {
#ifdef WIN32
  if (is_tty)
    WriteFile((HANDLE)_get_osfhandle(fileno(fout)), s, len, NULL, NULL);
  else
    fwrite(s, 1, len, fout);
#else
  fwrite(s, 1, len, fout);
#endif
  }
}

static void put_char(char c, FILE* fout, jv* strout, int T) {
  put_buf(&c, 1, fout, strout, T);
}

static void put_str(const char* s, FILE* fout, jv* strout, int T) {
  put_buf(s, strlen(s), fout, strout, T);
}

static void put_indent(int n, int flags, FILE* fout, jv* strout, int T) {
  if (flags & JV_PRINT_TAB) {
    while (n--)
      put_char('\t', fout, strout, T);
  } else {
    n *= ((flags & (JV_PRINT_SPACE0 | JV_PRINT_SPACE1 | JV_PRINT_SPACE2)) >> 8);
    while (n--)
      put_char(' ', fout, strout, T);
  }
}

static void jvp_dump_string(jv str, int ascii_only, FILE* F, jv* S, int T) {
  assert(jv_get_kind(str) == JV_KIND_STRING);
  const char* i = jv_string_value(str);
  const char* end = i + jv_string_length_bytes(jv_copy(str));
  const char* cstart;
  int c = 0;
  char buf[32];
  put_char('"', F, S, T);
  while ((i = jvp_utf8_next((cstart = i), end, &c))) {
    assert(c != -1);
    int unicode_escape = 0;
    if (0x20 <= c && c <= 0x7E) {
      // printable ASCII
      if (c == '"' || c == '\\') {
        put_char('\\', F, S, T);
      }
      put_char(c, F, S, T);
    } else if (c < 0x20 || c == 0x7F) {
      // ASCII control character
      switch (c) {
      case '\b':
        put_char('\\', F, S, T);
        put_char('b', F, S, T);
        break;
      case '\t':
        put_char('\\', F, S, T);
        put_char('t', F, S, T);
        break;
      case '\r':
        put_char('\\', F, S, T);
        put_char('r', F, S, T);
        break;
      case '\n':
        put_char('\\', F, S, T);
        put_char('n', F, S, T);
        break;
      case '\f':
        put_char('\\', F, S, T);
        put_char('f', F, S, T);
        break;
      default:
        unicode_escape = 1;
        break;
      }
    } else {
      if (ascii_only) {
        unicode_escape = 1;
      } else {
        put_buf(cstart, i - cstart, F, S, T);
      }
    }
    if (unicode_escape) {
      if (c <= 0xffff) {
        sprintf(buf, "\\u%04x", c);
      } else {
        c -= 0x10000;
        sprintf(buf, "\\u%04x\\u%04x", 
                0xD800 | ((c & 0xffc00) >> 10),
                0xDC00 | (c & 0x003ff));
      }
      put_str(buf, F, S, T);
    }
  }
  assert(c != -1);
  put_char('"', F, S, T);
}

static void put_refcnt(struct dtoa_context* C, int refcnt, FILE *F, jv* S, int T){
  char buf[JVP_DTOA_FMT_MAX_LEN];
  put_char(' ', F, S, T);
  put_char('(', F, S, T);
  put_str(jvp_dtoa_fmt(C, buf, refcnt), F, S, T);
  put_char(')', F, S, T);
}

static void jv_dump_term(struct dtoa_context* C, jv x, int flags, int indent, FILE* F, jv* S) {
  char buf[JVP_DTOA_FMT_MAX_LEN];
  const char* colour = 0;
  double refcnt = (flags & JV_PRINT_REFCOUNT) ? jv_get_refcnt(x) - 1 : -1;
  if (flags & JV_PRINT_COLOUR) {
    for (unsigned i=0; i<sizeof(colour_kinds)/sizeof(colour_kinds[0]); i++) {
      if (jv_get_kind(x) == colour_kinds[i]) {
        colour = colours[i];
        put_str(colour, F, S, flags & JV_PRINT_ISATTY);
        break;
      }
    }
  }
  switch (jv_get_kind(x)) {
  default:
  case JV_KIND_INVALID:
    if (flags & JV_PRINT_INVALID) {
      jv msg = jv_invalid_get_msg(jv_copy(x));
      if (jv_get_kind(msg) == JV_KIND_STRING) {
        put_str("<invalid:", F, S, flags & JV_PRINT_ISATTY);
        jvp_dump_string(msg, flags | JV_PRINT_ASCII, F, S, flags & JV_PRINT_ISATTY);
        put_str(">", F, S, flags & JV_PRINT_ISATTY);
      } else {
        put_str("<invalid>", F, S, flags & JV_PRINT_ISATTY);
      }
    } else {
      assert(0 && "Invalid value");
    }
    break;
  case JV_KIND_NULL:
    put_str("null", F, S, flags & JV_PRINT_ISATTY);
    break;
  case JV_KIND_FALSE:
    put_str("false", F, S, flags & JV_PRINT_ISATTY);
    break;
  case JV_KIND_TRUE:
    put_str("true", F, S, flags & JV_PRINT_ISATTY);
    break;
  case JV_KIND_NUMBER: {
    double d = jv_number_value(x);
    if (d != d) {
      // JSON doesn't have NaN, so we'll render it as "null"
      put_str("null", F, S, flags & JV_PRINT_ISATTY);
    } else {
      // Normalise infinities to something we can print in valid JSON
      if (d > DBL_MAX) d = DBL_MAX;
      if (d < -DBL_MAX) d = -DBL_MAX;
      put_str(jvp_dtoa_fmt(C, buf, d), F, S, flags & JV_PRINT_ISATTY);
    }
    break;
  }
  case JV_KIND_STRING:
    jvp_dump_string(x, flags & JV_PRINT_ASCII, F, S, flags & JV_PRINT_ISATTY);
    if (flags & JV_PRINT_REFCOUNT)
      put_refcnt(C, refcnt, F, S, flags & JV_PRINT_ISATTY);
    break;
  case JV_KIND_ARRAY: {
    if (jv_array_length(jv_copy(x)) == 0) {
      put_str("[]", F, S, flags & JV_PRINT_ISATTY);
      break;
    }
    put_str("[", F, S, flags & JV_PRINT_ISATTY);
    if (flags & JV_PRINT_PRETTY) {
      put_char('\n', F, S, flags & JV_PRINT_ISATTY);
      put_indent(indent + 1, flags, F, S, flags & JV_PRINT_ISATTY);
    }
    jv_array_foreach(x, i, elem) {
      if (i!=0) {
        if (flags & JV_PRINT_PRETTY) {
          put_str(",\n", F, S, flags & JV_PRINT_ISATTY);
          put_indent(indent + 1, flags, F, S, flags & JV_PRINT_ISATTY);
        } else {
          put_str(",", F, S, flags & JV_PRINT_ISATTY);
        }
      }
      jv_dump_term(C, elem, flags, indent + 1, F, S);
      if (colour) put_str(colour, F, S, flags & JV_PRINT_ISATTY);
    }
    if (flags & JV_PRINT_PRETTY) {
      put_char('\n', F, S, flags & JV_PRINT_ISATTY);
      put_indent(indent, flags, F, S, flags & JV_PRINT_ISATTY);
    }
    if (colour) put_str(colour, F, S, flags & JV_PRINT_ISATTY);
    put_char(']', F, S, flags & JV_PRINT_ISATTY);
    if (flags & JV_PRINT_REFCOUNT)
      put_refcnt(C, refcnt, F, S, flags & JV_PRINT_ISATTY);
    break;
  }
  case JV_KIND_OBJECT: {
    if (jv_object_length(jv_copy(x)) == 0) {
      put_str("{}", F, S, flags & JV_PRINT_ISATTY);
      break;
    }
    put_char('{', F, S, flags & JV_PRINT_ISATTY);
    if (flags & JV_PRINT_PRETTY) {
      put_char('\n', F, S, flags & JV_PRINT_ISATTY);
      put_indent(indent + 1, flags, F, S, flags & JV_PRINT_ISATTY);
    }
    int first = 1;
    int i = 0;
    jv keyset = jv_null();
    while (1) {
      jv key, value;
      if (flags & JV_PRINT_SORTED) {
        if (first) {
          keyset = jv_keys(jv_copy(x));
          i = 0;
        } else {
          i++;
        }
        if (i >= jv_array_length(jv_copy(keyset))) {
          jv_free(keyset);
          break;
        }
        key = jv_array_get(jv_copy(keyset), i);
        value = jv_object_get(jv_copy(x), jv_copy(key));
      } else {
        if (first) {
          i = jv_object_iter(x);
        } else {
          i = jv_object_iter_next(x, i);
        }
        if (!jv_object_iter_valid(x, i)) break;
        key = jv_object_iter_key(x, i);
        value = jv_object_iter_value(x, i);
      }

      if (!first) {
        if (flags & JV_PRINT_PRETTY){
          put_str(",\n", F, S, flags & JV_PRINT_ISATTY);
          put_indent(indent + 1, flags, F, S, flags & JV_PRINT_ISATTY);
        } else {
          put_str(",", F, S, flags & JV_PRINT_ISATTY);
        }
      }
      if (colour) put_str(COLRESET, F, S, flags & JV_PRINT_ISATTY);

      first = 0;
      if (colour) put_str(FIELD_COLOUR, F, S, flags & JV_PRINT_ISATTY);
      jvp_dump_string(key, flags & JV_PRINT_ASCII, F, S, flags & JV_PRINT_ISATTY);
      jv_free(key);
      if (colour) put_str(COLRESET, F, S, flags & JV_PRINT_ISATTY);

      if (colour) put_str(colour, F, S, flags & JV_PRINT_ISATTY);
      put_str((flags & JV_PRINT_PRETTY) ? ": " : ":", F, S, flags & JV_PRINT_ISATTY);
      if (colour) put_str(COLRESET, F, S, flags & JV_PRINT_ISATTY);
      
      jv_dump_term(C, value, flags, indent + 1, F, S);
      if (colour) put_str(colour, F, S, flags & JV_PRINT_ISATTY);
    }
    if (flags & JV_PRINT_PRETTY) {
      put_char('\n', F, S, flags & JV_PRINT_ISATTY);
      put_indent(indent, flags, F, S, flags & JV_PRINT_ISATTY);
    }
    if (colour) put_str(colour, F, S, flags & JV_PRINT_ISATTY);
    put_char('}', F, S, flags & JV_PRINT_ISATTY);
    if (flags & JV_PRINT_REFCOUNT)
      put_refcnt(C, refcnt, F, S, flags & JV_PRINT_ISATTY);
  }
  }
  jv_free(x);
  if (colour) {
    put_str(COLRESET, F, S, flags & JV_PRINT_ISATTY);
  }
}

void jv_dumpf(jv x, FILE *f, int flags) {
  struct dtoa_context C;
  jvp_dtoa_context_init(&C);
  jv_dump_term(&C, x, flags, 0, f, 0);
  jvp_dtoa_context_free(&C);
}

void jv_dump(jv x, int flags) {
  jv_dumpf(x, stdout, flags);
}

/* This one is nice for use in debuggers */
void jv_show(jv x, int flags) {
  if (flags == -1)
    flags = JV_PRINT_PRETTY | JV_PRINT_COLOUR | JV_PRINT_INDENT_FLAGS(2);
  jv_dumpf(jv_copy(x), stderr, flags | JV_PRINT_INVALID);
  fflush(stderr);
}

jv jv_dump_string(jv x, int flags) {
  struct dtoa_context C;
  jvp_dtoa_context_init(&C);
  jv s = jv_string("");
  jv_dump_term(&C, x, flags, 0, 0, &s);
  jvp_dtoa_context_free(&C);
  return s;
}

char *jv_dump_string_trunc(jv x, char *outbuf, size_t bufsize) {
  x = jv_dump_string(x,0);
  const char* p = jv_string_value(x);
  const size_t len = strlen(p);
  strncpy(outbuf, p, bufsize);
  outbuf[bufsize - 1] = 0;
  if (len > bufsize - 1 && bufsize >= 4) {
    // Indicate truncation with '...'
    outbuf[bufsize - 2]='.';
    outbuf[bufsize - 3]='.';
    outbuf[bufsize - 4]='.';
  }
  jv_free(x);
  return outbuf;
}
