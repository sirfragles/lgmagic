/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * json.h - minimal JSON parser/writer for the lg-magic tools (portable).
 *
 * Parser: recursive descent, numbers via strtod, depth limit 32, no
 * \uXXXX escapes (rejected with an error that reports a byte offset).
 * Writer: indent=4, "%.6f"-style floats - the same shape as Python's
 * json.dump(..., indent=4), which the original scripts used.
 */
#ifndef LG_TOOLS_JSON_H
#define LG_TOOLS_JSON_H

#include <stddef.h>
#include <stdbool.h>

enum json_type {
	JSON_NULL,
	JSON_BOOL,
	JSON_NUM,
	JSON_STR,
	JSON_ARR,
	JSON_OBJ
};

struct json_value {
	enum json_type type;
	union {
		bool boolean;
		double num;
		char *str;			/* NUL-terminated, owned */
		struct json_value **items;	/* array elements / object values */
	};
	char **keys;				/* object member names (NULL for arrays) */
	size_t count;
};

/* Parse text; on failure returns NULL and sets *err (message) / *err_off
 * (byte offset into the input). err/err_off may be NULL. */
struct json_value *json_parse(const char *text, size_t len,
			      const char **err, size_t *err_off);

struct json_value *json_obj_get(const struct json_value *obj, const char *key);
struct json_value *json_array_get(const struct json_value *arr, size_t i);
double json_array_get_num(const struct json_value *arr, size_t i, double dflt);

/* Convenience getters for the calibration JSON shape: a 3-float array and a
 * 3x3 array of arrays. Return 0 on success, -1 on malformed data. */
int json_get_float3(const struct json_value *obj, const char *key, double out[3]);
int json_get_mat3(const struct json_value *obj, const char *key, double out[3][3]);

void json_free(struct json_value *v);

/* Serialize a tree to a Python json.dump(indent=4)-shaped string.
 * Returns a malloc'd string (caller frees) or NULL on allocation failure. */
char *json_dumps(const struct json_value *v);

/* Tree construction (for calib_save_json / config_save_user).
 * All functions return the new value, or NULL on allocation failure.
 * On failure the partially-built tree is freed by the caller via
 * json_free() on the root. */
struct json_value *json_new(enum json_type type);
struct json_value *json_new_num(double d);
struct json_value *json_new_str(const char *s);
/* Append val to an object/array; the container takes ownership of val
 * on success. Returns 0 on success, -1 on allocation failure. */
int json_obj_add(struct json_value *obj, const char *key, struct json_value *val);
int json_arr_add(struct json_value *arr, struct json_value *val);

/* Parse a JSON file into a tree. Returns NULL on failure with err/err_off set. */
struct json_value *json_load_file(const char *path, const char **err, size_t *err_off);

#endif /* LG_TOOLS_JSON_H */
