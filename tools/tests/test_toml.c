/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * test_toml.c - unit tests for the minimal TOML parser/writer (toml.h).
 *
 * Covers: bare / quoted / dotted keys, [table] and [a.b] headers,
 * basic strings with escapes, literal strings, ints / floats / bools,
 * arrays of strings, comments, error line numbers, and the writer
 * round-trip through toml_parse(toml_dumps(x)).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test_util.h"
#include "toml.h"

static struct toml_value *parse(const char *text)
{
	const char *err = NULL;
	size_t line = 0;
	struct toml_value *v = toml_parse(text, strlen(text), &err, &line);

	if (!v)
		printf("  (parse failed: %s, line %zu)\n",
		       err ? err : "?", line);
	return v;
}

static void test_scalars(void)
{
	struct toml_value *v = parse(
		"# comment\n"
		"str = \"hello\"\n"
		"lit = 'literal\\nno escape'\n"
		"i = 42\n"
		"neg = -7\n"
		"big = 1_000\n"
		"f = 3.14\n"
		"fe = 2.5e-3\n"
		"b1 = true\n"
		"b2 = false\n");

	CHECK(v != NULL, "scalars parse");
	if (!v)
		return;
	CHECK_STR_EQ(toml_table_get_str(v, "str", NULL), "hello",
		      "basic string value");
	CHECK_STR_EQ(toml_table_get_str(v, "lit", NULL), "literal\\nno escape",
		      "literal string keeps backslashes");
	CHECK_INT_EQ((long long)toml_table_get_num(v, "i", -1), 42,
		      "integer value");
	CHECK_INT_EQ((long long)toml_table_get_num(v, "neg", 0), -7,
		      "negative integer");
	CHECK_INT_EQ((long long)toml_table_get_num(v, "big", 0), 1000,
		      "underscore digit separators");
	CHECK_FLOAT_EQ(toml_table_get_num(v, "f", 0), 3.14, "float value");
	CHECK_FLOAT_EQ(toml_table_get_num(v, "fe", 0), 0.0025,
			"float with exponent");
	CHECK_INT_EQ(toml_table_get_bool(v, "b1", 0), 1, "true parses");
	CHECK_INT_EQ(toml_table_get_bool(v, "b2", 1), 0, "false parses");
	toml_free(v);
}

static void test_strings(void)
{
	struct toml_value *v = parse(
		"s1 = \"esc\\\"ape\\\\\\n\\t\\r\\b\\f\"\n"
		"s2 = \"unicode \\u00E9 caf\\u00E9\"\n"
		"q = \"quoted key\"\n");

	CHECK(v != NULL, "escaped strings parse");
	if (!v)
		return;
	CHECK_STR_EQ(toml_table_get_str(v, "s1", NULL), "esc\"ape\\\n\t\r\b\f",
		      "basic escapes decode");
	CHECK_STR_EQ(toml_table_get_str(v, "s2", NULL), "unicode \xc3\xa9 caf\xc3\xa9",
		      "\\uXXXX decodes to UTF-8");
	CHECK_STR_EQ(toml_table_get_str(v, "q", NULL), "quoted key",
		      "quoted key works");
	toml_free(v);
}

static void test_tables(void)
{
	struct toml_value *v = parse(
		"a = 1\n"
		"[tbl]\n"
		"x = \"in tbl\"\n"
		"[tbl.sub]\n"
		"y = 2\n"
		"[other]\n"
		"z = 3\n"
		"# back to tbl: new key, allowed by TOML\n"
		"[tbl]\n"
		"w = true\n");

	CHECK(v != NULL, "tables parse");
	if (!v)
		return;
	CHECK_STR_EQ(toml_table_get_str(v, "tbl.x", NULL), "in tbl",
		      "nested table via dotted lookup");
	CHECK_INT_EQ((long long)toml_table_get_num(v, "tbl.sub.y", -1), 2,
		      "second-level nested table");
	CHECK_INT_EQ((long long)toml_table_get_num(v, "other.z", -1), 3,
		      "sibling table");
	CHECK_INT_EQ(toml_table_get_bool(v, "tbl.w", 0), 1,
		      "re-entering a table adds keys");
	CHECK_INT_EQ((long long)toml_table_get_num(v, "a", -1), 1,
		      "root key next to tables");
	CHECK(toml_table_get(v, "tbl") != NULL &&
	      toml_table_get(v, "tbl")->type == TOML_TABLE,
	      "table lookup returns a table");
	toml_free(v);
}

static void test_dotted_keys(void)
{
	struct toml_value *v = parse(
		"a.b.c = 1\n"
		"a.d = \"deep\"\n"
		"\"weird.key\".x = 5\n");

	CHECK(v != NULL, "dotted keys parse");
	if (!v)
		return;
	CHECK_INT_EQ((long long)toml_table_get_num(v, "a.b.c", -1), 1,
		      "dotted key creates nested tables");
	CHECK_STR_EQ(toml_table_get_str(v, "a.d", NULL), "deep",
		      "second dotted key in the same table");
	CHECK_INT_EQ((long long)toml_table_get_num(v, "\"weird.key\".x", -1), 5,
		      "quoted segment with a dot inside");
	toml_free(v);
}

static void test_arrays(void)
{
	struct toml_value *v = parse(
		"arr = [\"one\", \"two\", 'three']\n"
		"empty = []\n"
		"multi = [\n"
		"  \"a\", # trailing comment\n"
		"  \"b\",\n"
		"]\n");
	struct toml_value *arr;

	CHECK(v != NULL, "arrays parse");
	if (!v)
		return;
	arr = toml_table_get(v, "arr");
	CHECK(arr && arr->type == TOML_ARR && arr->count == 3,
	      "array has three elements");
	if (arr) {
		CHECK_STR_EQ(arr->items[0]->str, "one", "array element 0");
		CHECK_STR_EQ(arr->items[2]->str, "three",
			      "array element 2 (literal string)");
	}
	arr = toml_table_get(v, "empty");
	CHECK(arr && arr->type == TOML_ARR && arr->count == 0,
	      "empty array");
	arr = toml_table_get(v, "multi");
	CHECK(arr && arr->count == 2, "multi-line array with comments");
	toml_free(v);
}

static void test_errors(void)
{
	struct toml_value *v;
	const char *err = NULL;
	size_t line = 0;
	const char *unterminated = "a = 1\nb = \"unterminated\nc = 2\n";

	v = toml_parse(unterminated, strlen(unterminated), &err, &line);
	CHECK(v == NULL && err != NULL && line == 2,
	      "unterminated string errors with the line number");

	v = toml_parse("a = 1\na = 2\n", 12, &err, &line);
	CHECK(v == NULL && line == 2, "duplicate key errors");

	v = toml_parse("a = [1]\n", 8, &err, &line);
	CHECK(v == NULL, "non-string array element is rejected");

	v = toml_parse("a = [\"x\"\n", 9, &err, &line);
	CHECK(v == NULL, "unterminated array is rejected");

	v = toml_parse("[bad\n", 5, &err, &line);
	CHECK(v == NULL, "unterminated table header is rejected");

	v = toml_parse("a = \n", 5, &err, &line);
	CHECK(v == NULL, "missing value is rejected");

	v = toml_parse("a b = 1\n", 8, &err, &line);
	CHECK(v == NULL, "key without '=' is rejected");

	v = toml_parse("a = 1 junk\n", 11, &err, &line);
	CHECK(v == NULL, "trailing text after a value is rejected");

	v = toml_parse("", 0, &err, &line);
	CHECK(v != NULL, "empty input parses to an empty table");
	toml_free(v);

	v = toml_parse("a = 1\r\nb = 2\r\n", 13, &err, &line);
	CHECK(v != NULL, "CRLF line endings are accepted");
	toml_free(v);
}

static void test_round_trip(void)
{
	static const char *src =
		"imu_device = \"\"\n"
		"lpf_alpha = 0.2\n"
		"mouse_scale = 30.0\n"
		"madgwick_beta = 0.1\n"
		"alpha = 0.2\n"
		"mouse_k = 0.5\n"
		"gyro_scale_default = 0.07\n"
		"flag = true\n"
		"list = [\"a\", \"b\"]\n"
		"\n"
		"[devices.AA_BB_CC_DD_EE_FF]\n"
		"profile = \"default\"\n"
		"\n"
		"[devices.AA_BB_CC_DD_EE_FF.profiles.default.button_map]\n"
		"\"KEY_ENTER\" = \"BTN_LEFT\"\n";
	struct toml_value *v = parse(src);
	char *out;
	struct toml_value *v2;

	CHECK(v != NULL, "config-shaped document parses");
	if (!v)
		return;
	out = toml_dumps(v);
	CHECK(out != NULL, "toml_dumps produces output");
	if (!out) {
		toml_free(v);
		return;
	}
	v2 = toml_parse(out, strlen(out), NULL, NULL);
	CHECK(v2 != NULL, "dumped output re-parses");
	if (v2) {
		CHECK_FLOAT_EQ(toml_table_get_num(v2, "lpf_alpha", -1), 0.2,
				"round-trip float");
		CHECK_FLOAT_EQ(toml_table_get_num(v2, "mouse_scale", -1), 30.0,
				"round-trip integral float");
		CHECK_INT_EQ(toml_table_get_bool(v2, "flag", 0), 1,
			      "round-trip bool");
		CHECK_STR_EQ(toml_table_get_str(v2, "imu_device", "x"), "",
			      "round-trip empty string");
		CHECK_STR_EQ(toml_table_get_str(v2,
				"devices.AA_BB_CC_DD_EE_FF.profile", NULL),
			      "default", "round-trip nested table");
		CHECK_STR_EQ(toml_table_get_str(v2,
				"devices.AA_BB_CC_DD_EE_FF.profiles.default."
				"button_map.\"KEY_ENTER\"", NULL), "BTN_LEFT",
			      "round-trip quoted key in a nested table");
		toml_free(v2);
	}
	free(out);
	toml_free(v);
}

int main(void)
{
	TEST_BEGIN();
	test_scalars();
	test_strings();
	test_tables();
	test_dotted_keys();
	test_arrays();
	test_errors();
	test_round_trip();
	TEST_SUMMARY("test_toml");
	return tu_fail_count ? 1 : 0;
}
