/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * toml.c - minimal TOML parser/writer for the lg-magic tools (portable).
 * See toml.h for the supported subset.
 */
#include "toml.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Tree construction / destruction                                     */
/* ------------------------------------------------------------------ */

struct toml_value *toml_new_str(const char *s)
{
	struct toml_value *v = calloc(1, sizeof(*v));

	if (!v)
		return NULL;
	v->type = TOML_STR;
	v->str = strdup(s ? s : "");
	if (!v->str) {
		free(v);
		return NULL;
	}
	return v;
}

struct toml_value *toml_new_int(long long val)
{
	struct toml_value *v = calloc(1, sizeof(*v));

	if (!v)
		return NULL;
	v->type = TOML_INT;
	v->i = val;
	return v;
}

struct toml_value *toml_new_float(double val)
{
	struct toml_value *v = calloc(1, sizeof(*v));

	if (!v)
		return NULL;
	v->type = TOML_FLOAT;
	v->d = val;
	return v;
}

struct toml_value *toml_new_bool(int val)
{
	struct toml_value *v = calloc(1, sizeof(*v));

	if (!v)
		return NULL;
	v->type = TOML_BOOL;
	v->b = val != 0;
	return v;
}

static struct toml_value *toml_new_container(enum toml_type type)
{
	struct toml_value *v = calloc(1, sizeof(*v));

	if (!v)
		return NULL;
	v->type = type;
	return v;
}

struct toml_value *toml_new_arr(void)
{
	return toml_new_container(TOML_ARR);
}

struct toml_value *toml_new_table(void)
{
	return toml_new_container(TOML_TABLE);
}

int toml_table_add(struct toml_value *t, const char *key, struct toml_value *v)
{
	struct toml_value **items;
	char **keys;
	size_t i;

	if (!t || t->type != TOML_TABLE)
		return -1;
	for (i = 0; i < t->count; i++)
		if (strcmp(t->keys[i], key) == 0)
			return -1;	/* duplicate key */
	items = realloc(t->items, (t->count + 1) * sizeof(*items));
	if (!items)
		return -1;
	keys = realloc(t->keys, (t->count + 1) * sizeof(*keys));
	if (!keys)
		return -1;
	keys[t->count] = strdup(key);
	if (!keys[t->count])
		return -1;
	items[t->count] = v;
	t->items = items;
	t->keys = keys;
	t->count++;
	return 0;
}

int toml_arr_add(struct toml_value *arr, struct toml_value *v)
{
	struct toml_value **items;

	if (!arr || arr->type != TOML_ARR)
		return -1;
	items = realloc(arr->items, (arr->count + 1) * sizeof(*items));
	if (!items)
		return -1;
	items[arr->count] = v;
	arr->items = items;
	arr->count++;
	return 0;
}

void toml_free(struct toml_value *v)
{
	size_t i;

	if (!v)
		return;
	switch (v->type) {
	case TOML_STR:
		free(v->str);
		break;
	case TOML_ARR:
	case TOML_TABLE:
		for (i = 0; i < v->count; i++) {
			toml_free(v->items[i]);
			free(v->keys ? v->keys[i] : NULL);
		}
		free(v->items);
		free(v->keys);
		break;
	default:
		break;
	}
	free(v);
}

/* ------------------------------------------------------------------ */
/* Parser                                                              */
/* ------------------------------------------------------------------ */

struct parser {
	const char *p, *end;
	size_t line;
	const char *err;
	size_t err_line;
	struct toml_value *cur;		/* table the next keys belong to */
};

static void parse_error(struct parser *ps, const char *msg)
{
	if (!ps->err) {
		ps->err = msg;
		ps->err_line = ps->line;
	}
}

/* Like parse_error, but the error is reported at `line` regardless of
 * where the parser currently is (e.g. unterminated strings point at the
 * line they started on). */
static void parse_error_line(struct parser *ps, const char *msg, size_t line)
{
	if (!ps->err) {
		ps->err = msg;
		ps->err_line = line;
	}
}

/* Append `len` bytes from seq to the growing buffer.  Returns 1 on
 * success, 0 on allocation failure (used by parse_string below). */
static int store_seq(char **buf, size_t *n, size_t *cap, const char *seq,
		     size_t len);

static int peek(struct parser *ps, char c)
{
	return ps->p < ps->end && *ps->p == c;
}

/* Skip whitespace and comments; may cross newlines (line counter
 * updated).  Returns 0 on success, -1 on unterminated comment (EOF). */
static int skip_ws(struct parser *ps)
{
	for (;;) {
		while (ps->p < ps->end &&
		       (*ps->p == ' ' || *ps->p == '\t' || *ps->p == '\r'))
			ps->p++;
		if (peek(ps, '#')) {
			while (ps->p < ps->end && *ps->p != '\n')
				ps->p++;
			continue;
		}
		if (peek(ps, '\n')) {
			ps->p++;
			ps->line++;
			continue;
		}
		break;
	}
	return 0;
}

/* Consume the rest of a statement line: spaces/tabs and an optional
 * comment, then a newline or EOF.  Unlike skip_ws this never crosses
 * into the next line, so the trailing-text check sees the right thing.
 * Returns 0 on success, -1 on trailing garbage. */
static int skip_line_end(struct parser *ps)
{
	while (ps->p < ps->end &&
	       (*ps->p == ' ' || *ps->p == '\t' || *ps->p == '\r'))
		ps->p++;
	if (peek(ps, '#')) {
		while (ps->p < ps->end && *ps->p != '\n')
			ps->p++;
	}
	if (ps->p >= ps->end || peek(ps, '\n'))
		return 0;
	return -1;
}

/* Parse a quoted string (basic "..." or literal '...'); the opening
 * quote has been consumed by the caller and `quote` says which one. */
static char *parse_string(struct parser *ps, char quote)
{
	char *buf = NULL;
	size_t n = 0, cap = 0;
	size_t start_line = ps->line;

	for (;;) {
		char c;

		if (ps->p >= ps->end) {
			parse_error_line(ps, "unterminated string",
					 start_line);
			free(buf);
			return NULL;
		}
		c = *ps->p++;
		if (c == quote) {
			char *s = realloc(buf, n + 1);

			if (!s) {
				free(buf);
				return NULL;
			}
			s[n] = '\0';
			return s;
		}
		if (c == '\n') {
			ps->line++;
			if (quote == '\'') {
				/* Literal strings are single-line in TOML. */
				parse_error(ps, "newline in literal string");
				free(buf);
				return NULL;
			}
		}
		if (quote == '\'') {
			if (c != '\n' && (unsigned char)c < 0x20) {
				parse_error(ps, "control character in "
					    "literal string");
				free(buf);
				return NULL;
			}
			goto store;
		}
		/* Basic string: escapes. */
		if (c == '\\') {
			if (ps->p >= ps->end) {
				parse_error(ps, "unterminated escape");
				free(buf);
				return NULL;
			}
			c = *ps->p++;
			switch (c) {
			case 'n': c = '\n'; break;
			case 't': c = '\t'; break;
			case 'r': c = '\r'; break;
			case 'b': c = '\b'; break;
			case 'f': c = '\f'; break;
			case '"': c = '"'; break;
			case '\\': c = '\\'; break;
			case 'u':
				{
					unsigned v = 0;
					int k;

					for (k = 0; k < 4; k++) {
						if (ps->p >= ps->end) {
							parse_error(ps, "short "
								    "\\u escape");
							free(buf);
							return NULL;
						}
						c = *ps->p++;
						if (c >= '0' && c <= '9')
							v = v * 16 + (unsigned)(c - '0');
						else if (c >= 'a' && c <= 'f')
							v = v * 16 + (unsigned)(c - 'a' + 10);
						else if (c >= 'A' && c <= 'F')
							v = v * 16 + (unsigned)(c - 'A' + 10);
						else {
							parse_error(ps, "bad \\u "
								    "escape");
							free(buf);
							return NULL;
						}
					}
					/* UTF-8 encode; config strings are
					 * ASCII, so a simple encode is enough. */
					if (v < 0x80) {
						c = (char)v;
					} else if (v < 0x800) {
						char seq[2] = {
							(char)(0xC0 | (v >> 6)),
							(char)(0x80 | (v & 0x3F)),
						};

						if (!store_seq(&buf, &n, &cap, seq,
							       2)) {
							free(buf);
							return NULL;
						}
						continue;
					} else {
						char seq[3] = {
							(char)(0xE0 | (v >> 12)),
							(char)(0x80 | ((v >> 6) &
							       0x3F)),
							(char)(0x80 | (v & 0x3F)),
						};

						if (!store_seq(&buf, &n, &cap, seq,
							       3)) {
							free(buf);
							return NULL;
						}
						continue;
					}
				}
				break;
			default:
				parse_error(ps, "unknown escape");
				free(buf);
				return NULL;
			}
		} else if (c != '\t' && c != '\n' && (unsigned char)c < 0x20) {
			/* TOML forbids raw control chars in basic strings
			 * (newlines are fine - basic strings may span lines). */
			parse_error(ps, "control character in basic string");
			free(buf);
			return NULL;
		}
store:
		if (n == cap) {
			char *s;
			size_t ncap = cap ? cap * 2 : 32;

			s = realloc(buf, ncap);
			if (!s) {
				free(buf);
				return NULL;
			}
			buf = s;
			cap = ncap;
		}
		buf[n++] = c;
	}
}

static int store_seq(char **buf, size_t *n, size_t *cap, const char *seq,
		     size_t len)
{
	size_t i;

	for (i = 0; i < len; i++) {
		if (*n == *cap) {
			char *s;
			size_t ncap = *cap ? *cap * 2 : 32;

			s = realloc(*buf, ncap);
			if (!s)
				return 0;
			*buf = s;
			*cap = ncap;
		}
		(*buf)[(*n)++] = seq[i];
	}
	return 1;
}

/* Parse a bare key segment (no quotes): [A-Za-z0-9_-]+.  The caller
 * ensures the first character is bare-safe.  Returns malloc'd string. */
static char *parse_bare_key(struct parser *ps)
{
	const char *start = ps->p;

	while (ps->p < ps->end &&
	       (isalnum((unsigned char)*ps->p) || *ps->p == '_' ||
		*ps->p == '-'))
		ps->p++;
	if (ps->p == start)
		return NULL;
	{
		size_t len = (size_t)(ps->p - start);
		char *s = malloc(len + 1);

		if (!s)
			return NULL;
		memcpy(s, start, len);
		s[len] = '\0';
		return s;
	}
}

/* Parse one key segment (bare or quoted). */
static char *parse_key_segment(struct parser *ps)
{
	if (ps->p >= ps->end)
		return NULL;
	if (*ps->p == '"' || *ps->p == '\'') {
		char quote = *ps->p++;

		return parse_string(ps, quote);
	}
	if (isalnum((unsigned char)*ps->p) || *ps->p == '_' || *ps->p == '-')
		return parse_bare_key(ps);
	return NULL;
}

/* Parse a (possibly dotted) key path into an array of segments.
 * Returns the segment array (count in *nseg) or NULL on error. */
static char **parse_key_path(struct parser *ps, size_t *nseg)
{
	char **segs = NULL;
	size_t n = 0;

	for (;;) {
		char *seg = parse_key_segment(ps);
		char **s;

		if (!seg) {
			size_t i;

			for (i = 0; i < n; i++)
				free(segs[i]);
			free(segs);
			return NULL;
		}
		s = realloc(segs, (n + 1) * sizeof(*s));
		if (!s) {
			size_t i;

			free(seg);
			for (i = 0; i < n; i++)
				free(segs[i]);
			free(segs);
			return NULL;
		}
		segs = s;
		segs[n++] = seg;
		if (!peek(ps, '.'))
			break;
		ps->p++;
	}
	*nseg = n;
	return segs;
}

/* Walk into (creating as needed) the table addressed by segs[0..n-1].
 * Returns the table or NULL on error. */
static struct toml_value *enter(struct parser *ps, struct toml_value *root,
				char **segs, size_t n)
{
	struct toml_value *cur = root;
	size_t i;

	for (i = 0; i < n; i++) {
		struct toml_value *child = toml_table_get_short(cur, segs[i]);

		if (!child) {
			child = toml_new_table();
			if (!child || toml_table_add(cur, segs[i], child) < 0) {
				toml_free(child);
				return NULL;
			}
		}
		if (child->type != TOML_TABLE) {
			parse_error(ps, "key path crosses a non-table value");
			return NULL;
		}
		cur = child;
	}
	return cur;
}

/* Parse one value (after '=').  Strings, bools, numbers, arrays. */
static struct toml_value *parse_value(struct parser *ps)
{
	struct toml_value *v;

	if (ps->p >= ps->end) {
		parse_error(ps, "missing value");
		return NULL;
	}
	if (*ps->p == '"' || *ps->p == '\'') {
		char quote = *ps->p++;
		char *s = parse_string(ps, quote);

		if (!s)
			return NULL;
		v = toml_new_str(s);
		free(s);
		return v;
	}
	if (*ps->p == '[') {
		struct toml_value *arr = toml_new_arr();

		if (!arr)
			return NULL;
		ps->p++;
		for (;;) {
			char *s;

			skip_ws(ps);
			if (peek(ps, ']')) {
				ps->p++;
				return arr;
			}
			if (ps->p >= ps->end) {
				parse_error(ps, "unterminated array");
				toml_free(arr);
				return NULL;
			}
			if (*ps->p != '"' && *ps->p != '\'') {
				parse_error(ps, "arrays hold strings only");
				toml_free(arr);
				return NULL;
			}
			{
				char quote = *ps->p++;

				s = parse_string(ps, quote);
			}
			if (!s) {
				toml_free(arr);
				return NULL;
			}
			v = toml_new_str(s);
			free(s);
			if (!v || toml_arr_add(arr, v) < 0) {
				toml_free(v);
				toml_free(arr);
				return NULL;
			}
			skip_ws(ps);
			if (peek(ps, ',')) {
				ps->p++;
				continue;
			}
			if (peek(ps, ']')) {
				ps->p++;
				return arr;
			}
			parse_error(ps, "expected ',' or ']' in array");
			toml_free(arr);
			return NULL;
		}
	}
	if ((size_t)(ps->end - ps->p) >= 4 && memcmp(ps->p, "true", 4) == 0 &&
	    ((size_t)(ps->end - ps->p) == 4 ||
	     (!isalnum((unsigned char)ps->p[4]) && ps->p[4] != '_'))) {
		ps->p += 4;
		return toml_new_bool(1);
	}
	if ((size_t)(ps->end - ps->p) >= 5 && memcmp(ps->p, "false", 5) == 0 &&
	    ((size_t)(ps->end - ps->p) == 5 ||
	     (!isalnum((unsigned char)ps->p[5]) && ps->p[5] != '_'))) {
		ps->p += 5;
		return toml_new_bool(0);
	}
	{
		/* Number: [+-]? digits with optional '_' separators, '.',
		 * exponent.  Build a cleaned token (no '_') and classify by
		 * whether a '.' or 'e' appears. */
		char cleaned[64];
		size_t n = 0;
		int is_float = 0, digits = 0;

		if (peek(ps, '+') || peek(ps, '-')) {
			if (n + 1 < sizeof(cleaned))
				cleaned[n++] = *ps->p;
			ps->p++;
		}
		for (;;) {
			char c = ps->p < ps->end ? *ps->p : '\0';

			if (isdigit((unsigned char)c)) {
				digits++;
				if (n + 1 < sizeof(cleaned))
					cleaned[n++] = c;
				ps->p++;
			} else if (c == '_' && digits > 0 &&
				   ps->p + 1 < ps->end &&
				   isdigit((unsigned char)ps->p[1])) {
				ps->p++;	/* digit separator */
			} else if (c == '.') {
				if (n + 1 < sizeof(cleaned))
					cleaned[n++] = c;
				is_float = 1;
				ps->p++;
			} else if ((c == 'e' || c == 'E') && digits > 0) {
				if (n + 1 < sizeof(cleaned))
					cleaned[n++] = c;
				is_float = 1;
				ps->p++;
				if (ps->p < ps->end &&
				    (*ps->p == '+' || *ps->p == '-')) {
					if (n + 1 < sizeof(cleaned))
						cleaned[n++] = *ps->p;
					ps->p++;
				}
			} else {
				break;
			}
		}
		if (digits == 0 || n >= sizeof(cleaned)) {
			parse_error(ps, "invalid value");
			return NULL;
		}
		cleaned[n] = '\0';
		if (is_float) {
			char *end;
			double d = strtod(cleaned, &end);

			if (end == cleaned || *end != '\0') {
				parse_error(ps, "invalid float");
				return NULL;
			}
			return toml_new_float(d);
		} else {
			char *end;
			long long iv;

			errno = 0;
			iv = strtoll(cleaned, &end, 10);
			if (errno != 0 || end == cleaned || *end != '\0') {
				parse_error(ps, "invalid integer");
				return NULL;
			}
			return toml_new_int(iv);
		}
	}
}

struct toml_value *toml_parse(const char *text, size_t len, const char **err,
			      size_t *err_line)
{
	struct parser ps = { text, text + len, 1, NULL, 0, NULL };
	struct toml_value *root = toml_new_table();

	if (err)
		*err = NULL;
	if (err_line)
		*err_line = 0;
	if (!root)
		return NULL;
	ps.cur = root;
	while (ps.p < ps.end) {
		char **segs;
		size_t nseg = 0;

		skip_ws(&ps);
		if (ps.p >= ps.end)
			break;
		if (peek(&ps, '[')) {
			ps.p++;
			segs = parse_key_path(&ps, &nseg);
			if (!segs) {
				parse_error(&ps, "invalid table header");
				break;
			}
			if (!peek(&ps, ']')) {
				parse_error(&ps, "expected ']' in table "
					    "header");
				break;
			}
			ps.p++;
			if (skip_line_end(&ps) < 0) {
				parse_error(&ps, "trailing text after table "
					    "header");
				break;
			}
			ps.cur = enter(&ps, root, segs, nseg);
			{
				size_t i;

				for (i = 0; i < nseg; i++)
					free(segs[i]);
				free(segs);
			}
			if (!ps.cur)
				break;
			continue;
		}
		/* key = value */
		segs = parse_key_path(&ps, &nseg);
		if (!segs || nseg == 0) {
			parse_error(&ps, "expected key");
			break;
		}
		skip_ws(&ps);
		if (!peek(&ps, '=')) {
			parse_error(&ps, "expected '=' after key");
			break;
		}
		ps.p++;
		skip_ws(&ps);
		{
			struct toml_value *parent = enter(&ps, ps.cur, segs,
							  nseg - 1);
			struct toml_value *val;

			if (!parent) {
				size_t i;

				for (i = 0; i < nseg; i++)
					free(segs[i]);
				free(segs);
				break;
			}
			val = parse_value(&ps);
			if (!val) {
				size_t i;

				for (i = 0; i < nseg; i++)
					free(segs[i]);
				free(segs);
				break;
			}
			if (toml_table_add(parent, segs[nseg - 1], val) < 0) {
				parse_error(&ps, "duplicate key");
				toml_free(val);
			}
			{
				size_t i;

				for (i = 0; i < nseg; i++)
					free(segs[i]);
				free(segs);
			}
			if (skip_line_end(&ps) < 0) {
				parse_error(&ps, "trailing text after value");
				break;
			}
		}
	}
	if (ps.err) {
		if (err)
			*err = ps.err;
		if (err_line)
			*err_line = ps.err_line;
		toml_free(root);
		return NULL;
	}
	return root;
}

struct toml_value *toml_load_file(const char *path, const char **err,
				  size_t *err_line)
{
	static char open_err[160];
	struct toml_value *v;
	char *buf = NULL;
	size_t n = 0, cap = 0;
	FILE *f = fopen(path, "rb");

	if (!f) {
		if (err) {
			snprintf(open_err, sizeof(open_err), "cannot open file: "
				 "%s", strerror(errno));
			*err = open_err;
		}
		if (err_line)
			*err_line = 0;
		return NULL;
	}
	for (;;) {
		size_t got;

		if (n == cap) {
			char *s;
			size_t ncap = cap ? cap * 2 : 4096;

			s = realloc(buf, ncap);
			if (!s) {
				free(buf);
				fclose(f);
				return NULL;
			}
			buf = s;
			cap = ncap;
		}
		got = fread(buf + n, 1, cap - n, f);
		n += got;
		if (got == 0) {
			if (ferror(f)) {
				free(buf);
				fclose(f);
				return NULL;
			}
			break;
		}
	}
	fclose(f);
	v = toml_parse(buf, n, err, err_line);
	free(buf);
	return v;
}

/* ------------------------------------------------------------------ */
/* Lookup                                                              */
/* ------------------------------------------------------------------ */

struct toml_value *toml_table_get_short(const struct toml_value *t,
					const char *key)
{
	size_t i;

	if (!t || t->type != TOML_TABLE)
		return NULL;
	for (i = 0; i < t->count; i++)
		if (strcmp(t->keys[i], key) == 0)
			return t->items[i];
	return NULL;
}

/* Split the next segment off a dotted lookup path, honouring quotes the
 * same way parse_key_segment does (quotes stripped, \u escapes decoded).
 * Returns a malloc'd segment and advances *next past it, or NULL. */
static char *path_segment(const char *path, const char **next)
{
	const char *p = path;

	if (*p == '"' || *p == '\'') {
		char quote = *p++;
		char *buf = malloc(strlen(p) + 1);
		size_t n = 0;

		if (!buf)
			return NULL;
		while (*p && *p != quote) {
			char c = *p++;

			if (quote == '"' && c == '\\' && *p) {
				c = *p++;
				switch (c) {
				case 'n': c = '\n'; break;
				case 't': c = '\t'; break;
				case 'r': c = '\r'; break;
				case 'b': c = '\b'; break;
				case 'f': c = '\f'; break;
				case 'u':
					{
						unsigned v = 0;
						int k;

						for (k = 0; k < 4 && *p; k++) {
							char h = *p++;

							if (h >= '0' && h <= '9')
								v = v * 16 + (unsigned)(h - '0');
							else if (h >= 'a' && h <= 'f')
								v = v * 16 + (unsigned)(h - 'a' + 10);
							else if (h >= 'A' && h <= 'F')
								v = v * 16 + (unsigned)(h - 'A' + 10);
							else {
								free(buf);
								return NULL;
							}
						}
						if (k < 4) {
							free(buf);
							return NULL;
						}
						if (v < 0x80) {
							c = (char)v;
						} else if (v < 0x800) {
							buf[n++] = (char)(0xC0 | (v >> 6));
							buf[n++] = (char)(0x80 | (v & 0x3F));
							continue;
						} else {
							buf[n++] = (char)(0xE0 | (v >> 12));
							buf[n++] = (char)(0x80 | ((v >> 6) & 0x3F));
							buf[n++] = (char)(0x80 | (v & 0x3F));
							continue;
						}
					}
					break;
				default:
					break;
				}
			}
			buf[n++] = c;
		}
		if (!*p) {	/* unterminated quoted segment */
			free(buf);
			return NULL;
		}
		*next = p + 1;
		buf[n] = '\0';
		return buf;
	}
	{
		const char *dot = strchr(p, '.');
		size_t len = dot ? (size_t)(dot - p) : strlen(p);
		char *s;

		if (len == 0)
			return NULL;
		s = malloc(len + 1);
		if (!s)
			return NULL;
		memcpy(s, p, len);
		s[len] = '\0';
		*next = dot ? dot : p + len;
		return s;
	}
}

struct toml_value *toml_table_get(const struct toml_value *t,
				  const char *dotted)
{
	const struct toml_value *cur = t;

	while (cur && cur->type == TOML_TABLE) {
		const char *next;
		char *seg = path_segment(dotted, &next);
		struct toml_value *v;

		if (!seg)
			return NULL;
		v = toml_table_get_short(cur, seg);
		free(seg);
		if (!v)
			return NULL;
		if (!*next)
			return (struct toml_value *)v;
		if (*next != '.')
			return NULL;
		cur = v;
		dotted = next + 1;
	}
	return NULL;
}

const char *toml_table_get_str(const struct toml_value *t, const char *dotted,
			       const char *dflt)
{
	struct toml_value *v = toml_table_get(t, dotted);

	if (!v || v->type != TOML_STR)
		return dflt;
	return v->str;
}

double toml_table_get_num(const struct toml_value *t, const char *dotted,
			  double dflt)
{
	struct toml_value *v = toml_table_get(t, dotted);

	if (!v)
		return dflt;
	if (v->type == TOML_FLOAT)
		return v->d;
	if (v->type == TOML_INT)
		return (double)v->i;
	return dflt;
}

int toml_table_get_bool(const struct toml_value *t, const char *dotted,
			int dflt)
{
	struct toml_value *v = toml_table_get(t, dotted);

	if (!v || v->type != TOML_BOOL)
		return dflt;
	return v->b;
}

/* ------------------------------------------------------------------ */
/* Writer                                                              */
/* ------------------------------------------------------------------ */

static int bare_safe_key(const char *s)
{
	if (!*s)
		return 0;
	for (; *s; s++)
		if (!isalnum((unsigned char)*s) && *s != '_' && *s != '-')
			return 0;
	return 1;
}

static void dump_string(FILE *f, const char *s)
{
	fputc('"', f);
	for (; *s; s++) {
		unsigned char c = (unsigned char)*s;

		switch (c) {
		case '"': fputs("\\\"", f); break;
		case '\\': fputs("\\\\", f); break;
		case '\n': fputs("\\n", f); break;
		case '\t': fputs("\\t", f); break;
		case '\r': fputs("\\r", f); break;
		case '\b': fputs("\\b", f); break;
		case '\f': fputs("\\f", f); break;
		default:
			if (c < 0x20)
				fprintf(f, "\\u%04X", c);
			else
				fputc(c, f);
			break;
		}
	}
	fputc('"', f);
}

static void dump_path_key(FILE *f, const char *s)
{
	if (bare_safe_key(s))
		fputs(s, f);
	else
		dump_string(f, s);
}

static void dump_value(FILE *f, const struct toml_value *v)
{
	size_t i;

	switch (v->type) {
	case TOML_STR:
		dump_string(f, v->str);
		break;
	case TOML_INT:
		fprintf(f, "%lld", v->i);
		break;
	case TOML_FLOAT:
		{
			char num[64];

			snprintf(num, sizeof(num), "%.15g", v->d);
			fputs(num, f);
			/* integral values must keep a fractional part, or the
			 * document would re-parse them as TOML_INT */
			if (!strpbrk(num, ".eEn"))
				fputs(".0", f);
		}
		break;
	case TOML_BOOL:
		fputs(v->b ? "true" : "false", f);
		break;
	case TOML_ARR:
		fputc('[', f);
		for (i = 0; i < v->count; i++) {
			if (i)
				fputs(", ", f);
			dump_value(f, v->items[i]);
		}
		fputc(']', f);
		break;
	case TOML_TABLE:
		/* nested tables are emitted by dump_table, never inline */
		fputs("{}", f);
		break;
	}
}

/* Emit one table: scalar members first, then [header] + nested tables.
 * segs[0..nseg-1] are the parent path segments (quoted individually, so
 * a segment containing a dot survives the round trip). */
#define TOML_MAX_DEPTH 64

static int dump_table(FILE *f, const struct toml_value *t, char **segs,
		      size_t nseg)
{
	size_t i;
	int need_nl = 0;

	for (i = 0; i < t->count; i++) {
		if (t->items[i]->type == TOML_TABLE)
			continue;
		dump_path_key(f, t->keys[i]);
		fputs(" = ", f);
		dump_value(f, t->items[i]);
		fputc('\n', f);
		need_nl = 1;
	}
	for (i = 0; i < t->count; i++) {
		size_t j;

		if (t->items[i]->type != TOML_TABLE)
			continue;
		if (nseg >= TOML_MAX_DEPTH - 1)
			return -1;
		if (need_nl)
			fputc('\n', f);
		fputc('[', f);
		for (j = 0; j < nseg; j++) {
			if (j)
				fputc('.', f);
			dump_path_key(f, segs[j]);
		}
		if (nseg)
			fputc('.', f);
		dump_path_key(f, t->keys[i]);
		fputs("]\n", f);
		segs[nseg] = t->keys[i];
		if (dump_table(f, t->items[i], segs, nseg + 1) < 0)
			return -1;
		need_nl = 1;
	}
	return 0;
}

char *toml_dumps(const struct toml_value *v)
{
	FILE *f;
	char *buf = NULL;
	size_t n = 0;
	char *segs[TOML_MAX_DEPTH];

	if (!v || v->type != TOML_TABLE)
		return NULL;
	f = open_memstream(&buf, &n);
	if (!f)
		return NULL;
	if (dump_table(f, v, segs, 0) < 0 || fclose(f) != 0) {
		free(buf);
		return NULL;
	}
	return buf;
}
