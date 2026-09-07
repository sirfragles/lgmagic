/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * test_json.c - unit tests for json.h (minimal JSON parser/writer).
 *
 * Covers:
 *  - parsing a calibration-shaped JSON object and the getters
 *    (json_obj_get, json_get_float3, json_get_mat3, json_array_get_num);
 *  - parse error cases and their nonzero byte offsets (the position where
 *    the parser detected the problem): unclosed brace / string
 *    (offset == strlen, end of input), a comma where a value was expected
 *    (offset of the comma), trailing garbage (offset of the garbage byte),
 *    a malformed number token (offset just past the end of the scanned
 *    token), nesting deeper than the 32-level limit (one past the 33rd
 *    opening bracket), and rejected \uXXXX escapes (offset of the 'u');
 *  - json_dumps() / json_parse() round trips producing an equal tree;
 *  - numbers go through strtod ("-12.345679e2", "-0.0").
 */
#include <stdio.h>
#include <string.h>
#include <math.h>

#include "test_util.h"
#include "json.h"

/* Structural equality of two trees. */
static int json_equal(const struct json_value *a, const struct json_value *b)
{
	size_t i, j;

	if (!a || !b)
		return a == b;
	if (a->type != b->type)
		return 0;
	switch (a->type) {
	case JSON_NULL:
	case JSON_BOOL:
		return a->boolean == b->boolean;
	case JSON_NUM:
		return a->num == b->num;
	case JSON_STR:
		return strcmp(a->str, b->str) == 0;
	case JSON_ARR:
		if (a->count != b->count)
			return 0;
		for (i = 0; i < a->count; i++)
			if (!json_equal(a->items[i], b->items[i]))
				return 0;
		return 1;
	case JSON_OBJ:
		if (a->count != b->count)
			return 0;
		for (i = 0; i < a->count; i++) {
			/* keys are unordered: find the matching member */
			int found = 0;
			for (j = 0; j < b->count; j++) {
				if (strcmp(a->keys[i], b->keys[j]) == 0 &&
				    json_equal(a->items[i], b->items[j])) {
					found = 1;
					break;
				}
			}
			if (!found)
				return 0;
		}
		return 1;
	}
	return 0;
}

static void test_parse_valid_calibration(void)
{
	const char *text =
		"{"
		" \"accel\": {"
		"   \"bias\": [1.25, -2.5, 3.75],"
		"   \"matrix\": [[0.125, 0, 0.25], [-0.5, 0.75, 0.0625],"
		"                [0, 0.1875, 1.0]]"
		" },"
		" \"gyro\": {"
		"   \"bias\": [-10.5, 20.25, -30.75],"
		"   \"scale\": [1, 1, 1]"
		" },"
		" \"v1\": -12.345679e2,"
		" \"v2\": -0.0,"
		" \"note\": \"a\\\"b\\\\c\\nd\\te\","
		" \"flag\": true,"
		" \"nothing\": null"
		"}";
	const char *err = NULL;
	size_t err_off = 0;
	struct json_value *root, *accel, *gyro, *bias, *scale, *v, *note;
	double out3[3];
	double outm[3][3];
	int rc;
	size_t i, j;

	root = json_parse(text, strlen(text), &err, &err_off);
	CHECK(root != NULL, "valid calibration JSON parses");
	if (!root)
		return;
	CHECK(err == NULL && err_off == 0, "no error reported on valid input");
	CHECK(root->type == JSON_OBJ, "root value is an object");

	/* json_obj_get */
	accel = json_obj_get(root, "accel");
	CHECK(accel != NULL && accel->type == JSON_OBJ,
	      "json_obj_get(root, \"accel\") returns the accel object");
	gyro = json_obj_get(root, "gyro");
	CHECK(gyro != NULL && gyro->type == JSON_OBJ,
	      "json_obj_get(root, \"gyro\") returns the gyro object");
	CHECK(json_obj_get(root, "no_such_key") == NULL,
	      "json_obj_get of a missing key returns NULL");
	bias = json_obj_get(accel, "bias");
	CHECK(bias != NULL && bias->type == JSON_ARR && bias->count == 3,
	      "accel.bias is a 3-element array");
	CHECK(json_obj_get(bias, "x") == NULL,
	      "json_obj_get on an array returns NULL");

	/* json_array_get / json_array_get_num */
	CHECK(json_array_get(bias, 0) != NULL &&
	      json_array_get(bias, 0)->type == JSON_NUM,
	      "json_array_get(bias, 0) returns a number node");
	CHECK(json_array_get(bias, 3) == NULL &&
	      json_array_get(bias, 99) == NULL,
	      "json_array_get out of range returns NULL");
	CHECK(json_array_get_num(bias, 0, -999.0) == 1.25,
	      "json_array_get_num(bias, 0) == 1.25");
	CHECK(json_array_get_num(bias, 1, -999.0) == -2.5,
	      "json_array_get_num(bias, 1) == -2.5");
	CHECK(json_array_get_num(bias, 2, -999.0) == 3.75,
	      "json_array_get_num(bias, 2) == 3.75");
	CHECK(json_array_get_num(bias, 9, -999.0) == -999.0,
	      "json_array_get_num out of range returns the default");

	/* json_get_float3 / json_get_mat3 */
	rc = json_get_float3(accel, "bias", out3);
	CHECK(rc == 0 && out3[0] == 1.25 && out3[1] == -2.5 &&
	      out3[2] == 3.75, "json_get_float3 reads accel.bias");
	rc = json_get_mat3(accel, "matrix", outm);
	{
		static const double want[3][3] = {
			{ 0.125, 0.0, 0.25 },
			{ -0.5, 0.75, 0.0625 },
			{ 0.0, 0.1875, 1.0 }
		};
		int ok = rc == 0;
		for (i = 0; i < 3 && ok; i++)
			for (j = 0; j < 3; j++)
				if (outm[i][j] != want[i][j])
					ok = 0;
		CHECK(ok, "json_get_mat3 reads accel.matrix");
	}
	scale = json_obj_get(gyro, "scale");
	CHECK(scale != NULL && scale->count == 3 &&
	      json_array_get_num(scale, 1, -1.0) == 1.0,
	      "gyro.scale array reads back via json_array_get_num");

	/* numbers via strtod */
	v = json_obj_get(root, "v1");
	CHECK(v && v->type == JSON_NUM && v->num == strtod("-12.345679e2", NULL),
	      "number \"-12.345679e2\" parsed exactly like strtod");
	v = json_obj_get(root, "v2");
	CHECK(v && v->type == JSON_NUM && v->num == 0.0 && signbit(v->num),
	      "number \"-0.0\" keeps the negative sign bit");
	/* string content with escapes decoded */
	note = json_obj_get(root, "note");
	CHECK(note && note->type == JSON_STR &&
	      strcmp(note->str, "a\"b\\c\nd\te") == 0,
	      "string escapes decode (\\\", \\\\, \\n, \\t)");

	/* scalar top-level values parse to the obvious types */
	{
		struct json_value *t;
		t = json_parse("null", 4, NULL, NULL);
		CHECK(t && t->type == JSON_NULL, "\"null\" parses to JSON_NULL");
		json_free(t);
		t = json_parse("true", 4, NULL, NULL);
		CHECK(t && t->type == JSON_BOOL && t->boolean,
		      "\"true\" parses to JSON_BOOL");
		json_free(t);
		t = json_parse("false", 5, NULL, NULL);
		CHECK(t && t->type == JSON_BOOL && !t->boolean,
		      "\"false\" parses to JSON_BOOL=false");
		json_free(t);
		t = json_parse("[1, 2.5, -3]", 12, NULL, NULL);
		CHECK(t && t->type == JSON_ARR && t->count == 3 &&
		      json_array_get_num(t, 2, 0.0) == -3.0,
		      "flat number array parses");
		json_free(t);
		t = json_parse("{}", 2, NULL, NULL);
		CHECK(t && t->type == JSON_OBJ && t->count == 0,
		      "empty object parses");
		json_free(t);
		t = json_parse("[]", 2, NULL, NULL);
		CHECK(t && t->type == JSON_ARR && t->count == 0,
		      "empty array parses");
		json_free(t);
	}

	json_free(root);
}

static void expect_parse_error(const char *text, size_t want_off,
			       const char *msg)
{
	const char *err = NULL;
	size_t err_off = 0;
	struct json_value *v;

	v = json_parse(text, strlen(text), &err, &err_off);
	if (v) {
		json_free(v);
		CHECK(0, msg);
		return;
	}
	CHECK(err != NULL, msg);
	CHECK(err_off == want_off, msg);
	if (err_off != want_off)
		printf("  (%s: got offset %zu, want %zu)\n", msg,
		       err_off, want_off);
}

static void test_parse_errors(void)
{
	char deep[128];
	int i;

	/* unclosed brace: missing closing brace at the end of input */
	expect_parse_error("{\"accel\": {\"bias\": [1,2,3]}",
			   strlen("{\"accel\": {\"bias\": [1,2,3]}"),
			   "unclosed brace reports a nonzero byte offset");
	/* unclosed string */
	expect_parse_error("{\"a\":\"xyz", 9,
			   "unclosed string reports a nonzero byte offset");
	/* wrong type where a number is expected: ',' after ',' in an array */
	expect_parse_error("{\"bias\":[1,,3]}", 11,
			   "',' where a value was expected reports its offset");
	/* garbage after a complete value */
	expect_parse_error("{\"a\":1}x", 7,
			   "trailing garbage reports its offset");
	/* broken number token: strtod rejects "12e+", reported one past the
	 * end of the scanned token (the ']') */
	expect_parse_error("[12e+]", 5,
			   "broken number token reports a byte offset");
	/* \uXXXX escapes are rejected at the escape character */
	expect_parse_error("[\"\\u0041\"]", 3,
			   "\\uXXXX escape rejected with a byte offset");
	/* nesting deeper than 32 levels is rejected (error reported right
	 * after the 33rd '[' was consumed) */
	for (i = 0; i < 33; i++) {
		deep[i] = '[';
		deep[i + 33] = ']';
	}
	deep[66] = '\0';
	expect_parse_error(deep, 33,
			   "depth > 32 rejected with a byte offset");

	/* but 32 levels are accepted */
	{
		char ok[70];
		const char *err = NULL;
		size_t off = 0;
		struct json_value *v;
		for (i = 0; i < 32; i++) {
			ok[i] = '[';
			ok[i + 32] = ']';
		}
		ok[64] = '\0';
		v = json_parse(ok, 64, &err, &off);
		CHECK(v != NULL, "nesting of exactly 32 levels is accepted");
		json_free(v);
	}

	/* empty input is an error */
	{
		const char *err = NULL;
		struct json_value *v = json_parse("", 0, &err, NULL);
		CHECK(v == NULL && err != NULL, "empty input is an error");
		json_free(v);
	}
}

static void test_roundtrip(void)
{
	const char *text =
		"{\"accel\":{\"bias\":[1.25,-2.5,3.75],"
		"\"matrix\":[[0.125,0,0.25],[-0.5,0.75,0.0625],"
		"[0,0.1875,1]]},\"gyro\":{\"bias\":[-10.5,20.25,-30.75],"
		"\"scale\":[1,1,1]},\"v\":-12.345679e2,\"v2\":-0.0,"
		"\"note\":\"a\\\"b\\\\c\\nd\\te\",\"flag\":true,"
		"\"nothing\":null}";
	struct json_value *t1, *t2;
	char *s;
	const char *err = NULL;
	size_t off = 0;

	t1 = json_parse(text, strlen(text), NULL, NULL);
	CHECK(t1 != NULL, "compact calibration JSON parses");
	if (!t1)
		return;
	s = json_dumps(t1);
	CHECK(s != NULL, "json_dumps returns a string");
	if (s) {
		/* Python json.dump(..., indent=4) shape */
		CHECK(s[0] == '{' &&
		      strstr(s, "\n    \"accel\"") != NULL &&
		      s[strlen(s) - 1] == '}',
		      "json_dumps output is indent=4 shaped like "
		      "Python json.dump(indent=4)");
		t2 = json_parse(s, strlen(s), &err, &off);
		CHECK(t2 != NULL, "json_dumps output parses");
		if (t2) {
			CHECK(json_equal(t1, t2) == 1,
			      "round trip json_dumps(json_parse(s)) parses "
			      "back to an equal tree");
			json_free(t2);
		}
		free(s);
	}
	json_free(t1);
}

static void test_getter_errors(void)
{
	struct json_value *root;
	double out3[3];
	double outm[3][3];
	int rc;

	/* bias too short, not numeric, missing ... */
	root = json_parse(
		"{\"accel\":{\"bias\":[1,2],\"matrix\":[[1,2],[3,4],[5,6]]},"
		"\"gyro\":{\"bias\":[\"a\",\"b\",\"c\"]}}",
		strlen("{\"accel\":{\"bias\":[1,2],\"matrix\":[[1,2],[3,4],[5,6]]},"
		       "\"gyro\":{\"bias\":[\"a\",\"b\",\"c\"]}}"),
		NULL, NULL);
	CHECK(root != NULL, "malformed-for-getters JSON still parses");
	if (root) {
		struct json_value *accel = json_obj_get(root, "accel");
		struct json_value *gyro = json_obj_get(root, "gyro");
		rc = json_get_float3(accel, "bias", out3);
		CHECK(rc == -1, "json_get_float3 fails on a 2-element bias");
		rc = json_get_mat3(accel, "matrix", outm);
		CHECK(rc == -1, "json_get_mat3 fails on a 2x3 matrix");
		rc = json_get_mat3(accel, "bias", outm);
		CHECK(rc == -1, "json_get_mat3 fails when value is an array "
		      "of the wrong rank");
		rc = json_get_float3(accel, "missing", out3);
		CHECK(rc == -1, "json_get_float3 fails on a missing key");
		rc = json_get_mat3(gyro, "bias", outm);
		CHECK(rc == -1, "json_get_float3-style wrong-type member "
		      "fails (string array in place of numbers)");
		json_free(root);
	}
	{
		/* number where an object/array member name is expected */
		const char *err = NULL;
		struct json_value *v = json_parse("{1: 2}", 6, &err, NULL);
		CHECK(v == NULL && err != NULL,
		      "object member name must be a string");
		json_free(v);
	}
	{
		/* json_array_get_num default for a non-number element */
		struct json_value *v = json_parse("[1, \"x\", 3]", 11,
						  NULL, NULL);
		CHECK(v != NULL &&
		      json_array_get_num(v, 1, -777.0) == -777.0,
		      "json_array_get_num returns the default for a "
		      "non-number element");
		json_free(v);
	}
}

static void test_load_file(void)
{
	char path[4096];
	struct json_value *v;
	const char *err = NULL;
	size_t off = 0;
	const char *text = "{\"a\": [1, 2], \"b\": \"hi\"}\n";

	CHECK(tu_temp_path(path, sizeof(path), "json") == 0,
	      "can create a temp file for the file-load test");
	if (tu_write_file(path, text, strlen(text)) != 0) {
		CHECK(0, "can write the temp JSON file");
		return;
	}
	v = json_load_file(path, &err, &off);
	CHECK(v != NULL, "json_load_file reads a valid JSON file");
	if (v) {
		struct json_value *a = json_obj_get(v, "a");
		CHECK(a && a->count == 2 &&
		      json_array_get_num(a, 1, 0.0) == 2.0,
		      "file contents parse correctly");
		json_free(v);
	}
	remove(path);

	CHECK(json_load_file("/nonexistent/lgmagic-nope.json", &err, &off)
	      == NULL, "json_load_file fails on a missing file");
}

int main(void)
{
	TEST_BEGIN();
	test_parse_valid_calibration();
	test_parse_errors();
	test_roundtrip();
	test_getter_errors();
	test_load_file();
	TEST_SUMMARY("test_json");
	return tu_fail_count ? 1 : 0;
}
