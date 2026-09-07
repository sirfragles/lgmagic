/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * toml.h - minimal TOML parser/writer for the lg-magic tools (portable).
 *
 * v2 replaces the JSON config with TOML.  This is a deliberate subset:
 *   - bare / quoted / dotted keys, [table] and [a.b] headers,
 *   - basic strings with escapes, literal strings ('...'),
 *   - integers, floats, booleans, arrays of strings,
 *   - '#' comments.
 * No dates, no inline tables, no [[array of tables]] - the config schemas
 * never need them.
 *
 * Parser: line-based recursive descent; errors report the 1-based line
 * number.  Writer: emits tables as "key = value" lines with [header]
 * sections for nested tables; %.15g floats (round-trips doubles).
 */
#ifndef LG_TOOLS_TOML_H
#define LG_TOOLS_TOML_H

#include <stddef.h>

enum toml_type {
	TOML_STR,
	TOML_INT,
	TOML_FLOAT,
	TOML_BOOL,
	TOML_ARR,	/* array of strings only */
	TOML_TABLE,
};

struct toml_value {
	enum toml_type type;
	union {
		char *str;			/* NUL-terminated, owned */
		long long i;
		double d;
		int b;
		struct toml_value **items;	/* array elements / table values */
	};
	char **keys;				/* table member names (NULL for arrays) */
	size_t count;
};

/* Parse text; on failure returns NULL and sets *err (message) / *err_line
 * (1-based line number).  err/err_line may be NULL. */
struct toml_value *toml_parse(const char *text, size_t len,
			      const char **err, size_t *err_line);

/* Parse a file into a tree; NULL on failure with err/err_line set. */
struct toml_value *toml_load_file(const char *path, const char **err,
				  size_t *err_line);

void toml_free(struct toml_value *v);

/* Serialize a tree to a TOML document (malloc'd string, caller frees). */
char *toml_dumps(const struct toml_value *v);

/* Construction (for config_save_user / state files).  All return the new
 * value or NULL on allocation failure; on failure the partially-built
 * tree is freed by the caller via toml_free() on the root. */
struct toml_value *toml_new_str(const char *s);
struct toml_value *toml_new_int(long long v);
struct toml_value *toml_new_float(double v);
struct toml_value *toml_new_bool(int v);
struct toml_value *toml_new_arr(void);
struct toml_value *toml_new_table(void);
/* Append val to an object/array; the container takes ownership of val
 * on success.  Returns 0 on success, -1 on allocation failure or a
 * duplicate key (table). */
int toml_table_add(struct toml_value *t, const char *key, struct toml_value *v);
int toml_arr_add(struct toml_value *arr, struct toml_value *v);

/* Lookup helpers.  toml_table_get resolves "a.b.c" through nested tables
 * (intermediate segments must be tables); returns NULL when absent.
 * The typed getters treat TOML_INT and TOML_FLOAT as numbers and return
 * the default when the key is missing or the type does not match. */
struct toml_value *toml_table_get(const struct toml_value *t, const char *dotted);
struct toml_value *toml_table_get_short(const struct toml_value *t,
					const char *key);
const char *toml_table_get_str(const struct toml_value *t, const char *dotted,
			       const char *dflt);
double toml_table_get_num(const struct toml_value *t, const char *dotted,
			  double dflt);
int toml_table_get_bool(const struct toml_value *t, const char *dotted,
			int dflt);

#endif /* LG_TOOLS_TOML_H */
