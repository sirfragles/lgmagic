/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * json.c - minimal JSON parser/writer for the lg-magic tools (portable).
 *
 * Parser: recursive descent, numbers via strtod, depth limit 32, no
 * \uXXXX escapes (rejected with an error that reports a byte offset).
 * Writer: indent=4, floats formatted the way Python's repr() prints them
 * for the 6-decimal rounding used by the scripts - the same shape as
 * Python's json.dump(..., indent=4), which the original scripts used.
 */
#include "json.h"

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define JSON_MAX_DEPTH 32

/* ------------------------------------------------------------------ */
/* Parser                                                              */
/* ------------------------------------------------------------------ */

struct parser {
	const char *s;
	size_t len;
	size_t pos;
	unsigned depth;
	const char *err;
	size_t err_off;
};

static void perr(struct parser *p, const char *msg)
{
	if (!p->err) {
		p->err = msg;
		p->err_off = p->pos;
	}
}

static void skip_ws(struct parser *p)
{
	while (p->pos < p->len) {
		char c = p->s[p->pos];

		if (c != ' ' && c != '\t' && c != '\n' && c != '\r')
			break;
		p->pos++;
	}
}

static int parse_value(struct parser *p, struct json_value **out);

static struct json_value *vnew(enum json_type type)
{
	struct json_value *v = calloc(1, sizeof(*v));

	if (v)
		v->type = type;
	return v;
}

static int parse_string(struct parser *p, char **out)
{
	/* Parses a JSON string into a malloc'd NUL-terminated buffer.
	 * Returns 0 on success, -1 on error (err set on the parser). */
	size_t cap, n;
	char *buf;

	if (p->pos >= p->len || p->s[p->pos] != '"') {
		perr(p, "expected string");
		return -1;
	}
	p->pos++;
	cap = 16;
	n = 0;
	buf = malloc(cap);
	if (!buf) {
		perr(p, "out of memory");
		return -1;
	}
	while (p->pos < p->len) {
		char c = p->s[p->pos];

		if (c == '"') {
			p->pos++;
			if (n == cap && !(buf = realloc(buf, cap *= 2))) {
				perr(p, "out of memory");
				free(buf);
				return -1;
			}
			buf[n] = '\0';
			*out = buf;
			return 0;
		}
		if ((unsigned char)c < 0x20) {
			perr(p, "control character in string");
			free(buf);
			return -1;
		}
		if (c == '\\') {
			p->pos++;
			if (p->pos >= p->len) {
				perr(p, "unterminated escape");
				free(buf);
				return -1;
			}
			switch (p->s[p->pos]) {
			case '"': c = '"'; break;
			case '\\': c = '\\'; break;
			case '/': c = '/'; break;
			case 'b': c = '\b'; break;
			case 'f': c = '\f'; break;
			case 'n': c = '\n'; break;
			case 'r': c = '\r'; break;
			case 't': c = '\t'; break;
			case 'u':
				perr(p, "\\u escapes not supported");
				free(buf);
				return -1;
			default:
				perr(p, "invalid escape");
				free(buf);
				return -1;
			}
			p->pos++;
		} else {
			p->pos++;
		}
		if (n == cap && !(buf = realloc(buf, cap *= 2))) {
			perr(p, "out of memory");
			free(buf);
			return -1;
		}
		buf[n++] = c;
	}
	perr(p, "unterminated string");
	free(buf);
	return -1;
}

static int parse_number(struct parser *p, struct json_value **out)
{
	/* Called only from parse_value's default branch, so p->s[p->pos] is
	 * '-' or a digit and the scan below always consumes >= 1 char. */
	const char *start = p->s + p->pos;
	char *end;
	double d;

	while (p->pos < p->len &&
	       (isdigit((unsigned char)p->s[p->pos]) ||
		p->s[p->pos] == '-' || p->s[p->pos] == '+' ||
		p->s[p->pos] == '.' || p->s[p->pos] == 'e' ||
		p->s[p->pos] == 'E'))
		p->pos++;
	/* Reject tokens with no digit at all (e.g. "-", ".") */
	{
		const char *q;
		int has_digit = 0;

		for (q = start; q < p->s + p->pos; q++)
			if (isdigit((unsigned char)*q)) {
				has_digit = 1;
				break;
			}
		if (!has_digit) {
			perr(p, "invalid number");
			return -1;
		}
	}
	errno = 0;
	d = strtod(start, &end);
	if (errno == ERANGE || end != p->s + p->pos) {
		perr(p, "invalid number");
		return -1;
	}
	*out = vnew(JSON_NUM);
	if (!*out) {
		perr(p, "out of memory");
		return -1;
	}
	(*out)->num = d;
	return 0;
}

static int parse_array(struct parser *p, struct json_value **out)
{
	struct json_value *v = vnew(JSON_ARR);

	*out = v;
	if (!v) {
		perr(p, "out of memory");
		return -1;
	}
	p->pos++;		/* '[' */
	if (++p->depth > JSON_MAX_DEPTH) {
		perr(p, "nesting too deep");
		return -1;
	}
	skip_ws(p);
	if (p->pos < p->len && p->s[p->pos] == ']') {
		p->pos++;
		p->depth--;
		return 0;
	}
	for (;;) {
		struct json_value *item = NULL;

		if (parse_value(p, &item) < 0)
			return -1;
		if (json_arr_add(v, item) < 0) {
			json_free(item);
			perr(p, "out of memory");
			return -1;
		}
		skip_ws(p);
		if (p->pos >= p->len) {
			perr(p, "unterminated array");
			return -1;
		}
		if (p->s[p->pos] == ',') {
			p->pos++;
			skip_ws(p);
			continue;
		}
		if (p->s[p->pos] == ']') {
			p->pos++;
			p->depth--;
			return 0;
		}
		perr(p, "expected ',' or ']'");
		return -1;
	}
}

static int parse_object(struct parser *p, struct json_value **out)
{
	struct json_value *v = vnew(JSON_OBJ);

	*out = v;
	if (!v) {
		perr(p, "out of memory");
		return -1;
	}
	p->pos++;		/* '{' */
	if (++p->depth > JSON_MAX_DEPTH) {
		perr(p, "nesting too deep");
		return -1;
	}
	skip_ws(p);
	if (p->pos < p->len && p->s[p->pos] == '}') {
		p->pos++;
		p->depth--;
		return 0;
	}
	for (;;) {
		char *key = NULL;
		struct json_value *item = NULL;

		if (parse_string(p, &key) < 0)
			return -1;
		skip_ws(p);
		if (p->pos >= p->len || p->s[p->pos] != ':') {
			perr(p, "expected ':'");
			free(key);
			return -1;
		}
		p->pos++;
		skip_ws(p);
		if (parse_value(p, &item) < 0) {
			free(key);
			return -1;
		}
		if (json_obj_add(v, key, item) < 0) {
			free(key);
			json_free(item);
			perr(p, "out of memory");
			return -1;
		}
		free(key);
		skip_ws(p);
		if (p->pos >= p->len) {
			perr(p, "unterminated object");
			return -1;
		}
		if (p->s[p->pos] == ',') {
			p->pos++;
			skip_ws(p);
			continue;
		}
		if (p->s[p->pos] == '}') {
			p->pos++;
			p->depth--;
			return 0;
		}
		perr(p, "expected ',' or '}'");
		return -1;
	}
}

static int parse_value(struct parser *p, struct json_value **out)
{
	skip_ws(p);
	if (p->pos >= p->len) {
		perr(p, "unexpected end of input");
		return -1;
	}
	switch (p->s[p->pos]) {
	case '{':
		return parse_object(p, out);
	case '[':
		return parse_array(p, out);
	case '"':
		{
			char *s = NULL;
			struct json_value *v;

			if (parse_string(p, &s) < 0)
				return -1;
			v = vnew(JSON_STR);
			if (!v) {
				free(s);
				perr(p, "out of memory");
				return -1;
			}
			v->str = s;
			*out = v;
			return 0;
		}
	case 't':
		if (strncmp(p->s + p->pos, "true", 4) == 0) {
			p->pos += 4;
			*out = vnew(JSON_BOOL);
			if (!*out)
				goto oom;
			(*out)->boolean = true;
			return 0;
		}
		perr(p, "expected value");
		return -1;
	case 'f':
		if (strncmp(p->s + p->pos, "false", 5) == 0) {
			p->pos += 5;
			*out = vnew(JSON_BOOL);
			if (!*out)
				goto oom;
			(*out)->boolean = false;
			return 0;
		}
		perr(p, "expected value");
		return -1;
	case 'n':
		if (strncmp(p->s + p->pos, "null", 4) == 0) {
			p->pos += 4;
			*out = vnew(JSON_NULL);
			if (!*out)
				goto oom;
			return 0;
		}
		perr(p, "expected value");
		return -1;
	default:
		if (p->s[p->pos] == '-' ||
		    isdigit((unsigned char)p->s[p->pos]))
			return parse_number(p, out);
		perr(p, "expected value");
		return -1;
	}
oom:
	perr(p, "out of memory");
	return -1;
}

struct json_value *json_parse(const char *text, size_t len,
			      const char **err, size_t *err_off)
{
	struct parser p = { .s = text, .len = len };
	struct json_value *root = NULL;

	if (parse_value(&p, &root) < 0) {
		if (err)
			*err = p.err;
		if (err_off)
			*err_off = p.err_off;
		json_free(root);
		return NULL;
	}
	skip_ws(&p);
	if (p.pos != p.len) {
		if (err)
			*err = "trailing data after JSON value";
		if (err_off)
			*err_off = p.pos;
		json_free(root);
		return NULL;
	}
	return root;
}

/* ------------------------------------------------------------------ */
/* Accessors                                                           */
/* ------------------------------------------------------------------ */

struct json_value *json_obj_get(const struct json_value *obj, const char *key)
{
	size_t i;

	if (!obj || obj->type != JSON_OBJ)
		return NULL;
	for (i = 0; i < obj->count; i++)
		if (strcmp(obj->keys[i], key) == 0)
			return obj->items[i];
	return NULL;
}

struct json_value *json_array_get(const struct json_value *arr, size_t i)
{
	if (!arr || arr->type != JSON_ARR || i >= arr->count)
		return NULL;
	return arr->items[i];
}

double json_array_get_num(const struct json_value *arr, size_t i, double dflt)
{
	struct json_value *v = json_array_get(arr, i);

	if (!v || v->type != JSON_NUM)
		return dflt;
	return v->num;
}

int json_get_float3(const struct json_value *obj, const char *key, double out[3])
{
	struct json_value *v = json_obj_get(obj, key);
	int i;

	if (!v || v->type != JSON_ARR || v->count != 3)
		return -1;
	for (i = 0; i < 3; i++) {
		if (v->items[i]->type != JSON_NUM)
			return -1;
		out[i] = v->items[i]->num;
	}
	return 0;
}

int json_get_mat3(const struct json_value *obj, const char *key,
		  double out[3][3])
{
	struct json_value *v = json_obj_get(obj, key);
	int i, j;

	if (!v || v->type != JSON_ARR || v->count != 3)
		return -1;
	for (i = 0; i < 3; i++) {
		struct json_value *row = v->items[i];

		if (row->type != JSON_ARR || row->count != 3)
			return -1;
		for (j = 0; j < 3; j++) {
			if (row->items[j]->type != JSON_NUM)
				return -1;
			out[i][j] = row->items[j]->num;
		}
	}
	return 0;
}

void json_free(struct json_value *v)
{
	size_t i;

	if (!v)
		return;
	if (v->type == JSON_STR)
		free(v->str);
	if (v->type == JSON_ARR || v->type == JSON_OBJ) {
		for (i = 0; i < v->count; i++) {
			if (v->type == JSON_OBJ)
				free(v->keys[i]);
			json_free(v->items[i]);
		}
		free(v->keys);
		free(v->items);
	}
	free(v);
}

/* ------------------------------------------------------------------ */
/* Construction                                                        */
/* ------------------------------------------------------------------ */

struct json_value *json_new(enum json_type type)
{
	return vnew(type);
}

struct json_value *json_new_num(double d)
{
	struct json_value *v = vnew(JSON_NUM);

	if (v)
		v->num = d;
	return v;
}

struct json_value *json_new_str(const char *s)
{
	struct json_value *v = vnew(JSON_STR);

	if (v) {
		v->str = strdup(s);
		if (!v->str) {
			free(v);
			return NULL;
		}
	}
	return v;
}

int json_arr_add(struct json_value *arr, struct json_value *val)
{
	struct json_value **items;

	if (!arr || arr->type != JSON_ARR || !val)
		return -1;
	items = realloc(arr->items, (arr->count + 1) * sizeof(*items));
	if (!items)
		return -1;
	arr->items = items;
	arr->items[arr->count++] = val;
	return 0;
}

int json_obj_add(struct json_value *obj, const char *key, struct json_value *val)
{
	struct json_value **items;
	char **keys, *key_copy;

	if (!obj || obj->type != JSON_OBJ || !val)
		return -1;
	key_copy = strdup(key);
	if (!key_copy)
		return -1;
	items = realloc(obj->items, (obj->count + 1) * sizeof(*items));
	keys = realloc(obj->keys, (obj->count + 1) * sizeof(*keys));
	if (!items || !keys) {
		free(key_copy);
		/* If only one realloc succeeded, shrink it back so the
		 * container stays consistent (count is unchanged). */
		if (items)
			obj->items = realloc(items, obj->count * sizeof(*items));
		if (keys)
			obj->keys = realloc(keys, obj->count * sizeof(*keys));
		return -1;
	}
	obj->items = items;
	obj->keys = keys;
	obj->keys[obj->count] = key_copy;
	obj->items[obj->count] = val;
	obj->count++;
	return 0;
}

/* ------------------------------------------------------------------ */
/* Writer                                                              */
/* ------------------------------------------------------------------ */

struct sbuf {
	char *p;
	size_t len;
	size_t cap;
};

static int sb_reserve(struct sbuf *b, size_t extra)
{
	size_t need = b->len + extra;
	char *np;

	if (need <= b->cap)
		return 0;
	if (b->cap == 0)
		b->cap = 64;
	while (b->cap < need)
		b->cap *= 2;
	np = realloc(b->p, b->cap);
	if (!np)
		return -1;
	b->p = np;
	return 0;
}

static int sb_putc(struct sbuf *b, char c)
{
	if (sb_reserve(b, 1) < 0)
		return -1;
	b->p[b->len++] = c;
	return 0;
}

static int sb_puts(struct sbuf *b, const char *s)
{
	size_t n = strlen(s);

	if (sb_reserve(b, n) < 0)
		return -1;
	memcpy(b->p + b->len, s, n);
	b->len += n;
	return 0;
}

static int sb_putspaces(struct sbuf *b, int n)
{
	int i;

	if (sb_reserve(b, (size_t)n) < 0)
		return -1;
	for (i = 0; i < n; i++)
		b->p[b->len++] = ' ';
	return 0;
}

/* Format a float the way Python's repr() prints the 6-decimal rounded
 * value: %.6f, trailing zeros after the point stripped, ".0" kept.
 * Falls back to %g for values Python would print in scientific notation
 * (abs < 5e-7 or >= 1e16) - the calibration files never contain those. */
static void fmt_number(char *buf, size_t bufsz, double d)
{
	size_t n;

	if (isnan(d) || isinf(d)) {
		/* Python json.dump would refuse these; never happens here. */
		snprintf(buf, bufsz, "null");
		return;
	}
	if (d != 0.0 && (fabs(d) < 5e-7 || fabs(d) >= 1e16)) {
		snprintf(buf, bufsz, "%g", d);
		return;
	}
	snprintf(buf, bufsz, "%.6f", d);
	n = strlen(buf);
	if (strchr(buf, '.')) {
		while (n > 1 && buf[n - 1] == '0')
			buf[--n] = '\0';
		if (buf[n - 1] == '.')
			buf[n++] = '0';
		buf[n] = '\0';
	}
}

static void escape_string(struct sbuf *b, const char *s)
{
	sb_putc(b, '"');
	for (; *s; s++) {
		switch (*s) {
		case '"': sb_puts(b, "\\\""); break;
		case '\\': sb_puts(b, "\\\\"); break;
		case '\b': sb_puts(b, "\\b"); break;
		case '\f': sb_puts(b, "\\f"); break;
		case '\n': sb_puts(b, "\\n"); break;
		case '\r': sb_puts(b, "\\r"); break;
		case '\t': sb_puts(b, "\\t"); break;
		default:
			if ((unsigned char)*s < 0x20) {
				char u[7];

				snprintf(u, sizeof(u), "\\u%04x", *s);
				sb_puts(b, u);
			} else {
				sb_putc(b, *s);
			}
		}
	}
	sb_putc(b, '"');
}

static int dump_value(struct sbuf *b, const struct json_value *v, int level)
{
	size_t i;

	switch (v->type) {
	case JSON_NULL:
		return sb_puts(b, "null");
	case JSON_BOOL:
		return sb_puts(b, v->boolean ? "true" : "false");
	case JSON_NUM:
		{
			char buf[48];

			fmt_number(buf, sizeof(buf), v->num);
			return sb_puts(b, buf);
		}
	case JSON_STR:
		escape_string(b, v->str);
		return 0;
	case JSON_ARR:
		if (sb_putc(b, '[') < 0)
			return -1;
		if (v->count == 0)
			return sb_putc(b, ']');
		for (i = 0; i < v->count; i++) {
			if (sb_putc(b, '\n') < 0 ||
			    sb_putspaces(b, 4 * (level + 1)) < 0 ||
			    dump_value(b, v->items[i], level + 1) < 0)
				return -1;
			if (i + 1 < v->count && sb_putc(b, ',') < 0)
				return -1;
		}
		if (sb_putc(b, '\n') < 0 || sb_putspaces(b, 4 * level) < 0)
			return -1;
		return sb_putc(b, ']');
	case JSON_OBJ:
		if (sb_putc(b, '{') < 0)
			return -1;
		if (v->count == 0)
			return sb_putc(b, '}');
		for (i = 0; i < v->count; i++) {
			if (sb_putc(b, '\n') < 0 ||
			    sb_putspaces(b, 4 * (level + 1)) < 0)
				return -1;
			escape_string(b, v->keys[i]);
			if (sb_puts(b, ": ") < 0 ||
			    dump_value(b, v->items[i], level + 1) < 0)
				return -1;
			if (i + 1 < v->count && sb_putc(b, ',') < 0)
				return -1;
		}
		if (sb_putc(b, '\n') < 0 || sb_putspaces(b, 4 * level) < 0)
			return -1;
		return sb_putc(b, '}');
	}
	return -1;
}

char *json_dumps(const struct json_value *v)
{
	struct sbuf b = { 0 };
	char *out;

	if (!v || dump_value(&b, v, 0) < 0 || sb_putc(&b, '\0') < 0) {
		free(b.p);
		return NULL;
	}
	out = realloc(b.p, b.len);
	return out ? out : b.p;
}

struct json_value *json_load_file(const char *path, const char **err,
				  size_t *err_off)
{
	FILE *f;
	long sz;
	char *buf;
	struct json_value *root;

	f = fopen(path, "rb");
	if (!f) {
		if (err)
			*err = strerror(errno);
		if (err_off)
			*err_off = 0;
		return NULL;
	}
	if (fseek(f, 0, SEEK_END) < 0 || (sz = ftell(f)) < 0 ||
	    fseek(f, 0, SEEK_SET) < 0) {
		fclose(f);
		if (err)
			*err = "cannot read file";
		if (err_off)
			*err_off = 0;
		return NULL;
	}
	buf = malloc((size_t)sz + 1);
	if (!buf) {
		fclose(f);
		if (err)
			*err = "out of memory";
		if (err_off)
			*err_off = 0;
		return NULL;
	}
	if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
		fclose(f);
		free(buf);
		if (err)
			*err = "cannot read file";
		if (err_off)
			*err_off = 0;
		return NULL;
	}
	fclose(f);
	buf[sz] = '\0';
	root = json_parse(buf, (size_t)sz, err, err_off);
	free(buf);
	return root;
}
