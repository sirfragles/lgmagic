/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * test_util.h - minimal assertion harness for the lg-magic unit tests.
 *
 * Plain C11, zero external dependencies.  Each test file is a standalone
 * program that prints one "PASS <msg>" / "FAIL <msg> [file:line]" line per
 * check and finishes with a summary line like
 *
 *     test_json: 42 passed, 0 failed
 *
 * and exits nonzero if anything failed.
 *
 * File/stdio helpers shared by several tests (temp files, whole-file
 * read/write, recursive rm, mkdir -p) live here as static functions marked
 * unused-safe so that -Wall -Wextra builds do not complain about helpers a
 * given test does not need.
 */
#ifndef LG_TOOLS_TESTS_TEST_UTIL_H
#define LG_TOOLS_TESTS_TEST_UTIL_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <limits.h>
#include <errno.h>
#include <unistd.h>
#include <sys/stat.h>
#include <dirent.h>

#if defined(__GNUC__) || defined(__clang__)
#define TU_UNUSED __attribute__((unused))
#else
#define TU_UNUSED
#endif

static int tu_pass_count TU_UNUSED;
static int tu_fail_count TU_UNUSED;

#define TEST_BEGIN() do { tu_pass_count = 0; tu_fail_count = 0; } while (0)

#define CHECK(cond, msg) do { \
	if (cond) { \
		tu_pass_count++; \
		printf("PASS %s\n", (msg)); \
	} else { \
		tu_fail_count++; \
		printf("FAIL %s [%s:%d]\n", (msg), __FILE__, __LINE__); \
	} \
} while (0)

/* Exact equality of integers (any integer type, printed as long long). */
#define CHECK_INT_EQ(actual, expected, msg) do { \
	long long tu_a_ = (long long)(actual); \
	long long tu_b_ = (long long)(expected); \
	if (tu_a_ == tu_b_) { \
		tu_pass_count++; \
		printf("PASS %s\n", (msg)); \
	} else { \
		tu_fail_count++; \
		printf("FAIL %s: got %lld, want %lld [%s:%d]\n", \
		       (msg), tu_a_, tu_b_, __FILE__, __LINE__); \
	} \
} while (0)

/* Exact string equality. */
#define CHECK_STR_EQ(actual, expected, msg) do { \
	const char *tu_a_ = (actual); \
	const char *tu_b_ = (expected); \
	if (tu_a_ && tu_b_ && strcmp(tu_a_, tu_b_) == 0) { \
		tu_pass_count++; \
		printf("PASS %s\n", (msg)); \
	} else { \
		tu_fail_count++; \
		printf("FAIL %s: got \"%s\", want \"%s\" [%s:%d]\n", \
		       (msg), tu_a_ ? tu_a_ : "(null)", \
		       tu_b_ ? tu_b_ : "(null)", __FILE__, __LINE__); \
	} \
} while (0)

/* Exact float/double equality (NaN never passes). */
#define CHECK_FLOAT_EQ(actual, expected, msg) do { \
	double tu_a_ = (double)(actual); \
	double tu_b_ = (double)(expected); \
	if (tu_a_ == tu_b_) { \
		tu_pass_count++; \
		printf("PASS %s\n", (msg)); \
	} else { \
		tu_fail_count++; \
		printf("FAIL %s: got %.17g, want %.17g [%s:%d]\n", \
		       (msg), tu_a_, tu_b_, __FILE__, __LINE__); \
	} \
} while (0)

/* Approximate float/double equality: |a - b| <= tol. */
#define CHECK_FLOAT_NEAR(actual, expected, tol, msg) do { \
	double tu_a_ = (double)(actual); \
	double tu_b_ = (double)(expected); \
	double tu_t_ = (double)(tol); \
	if (fabs(tu_a_ - tu_b_) <= tu_t_) { \
		tu_pass_count++; \
		printf("PASS %s\n", (msg)); \
	} else { \
		tu_fail_count++; \
		printf("FAIL %s: got %.17g, want %.17g (tol %.3g) [%s:%d]\n", \
		       (msg), tu_a_, tu_b_, tu_t_, __FILE__, __LINE__); \
	} \
} while (0)

/* Equal pointer values (either both NULL or both the same address). */
#define CHECK_PTR_EQ(actual, expected, msg) do { \
	const void *tu_a_ = (const void *)(actual); \
	const void *tu_b_ = (const void *)(expected); \
	if (tu_a_ == tu_b_) { \
		tu_pass_count++; \
		printf("PASS %s\n", (msg)); \
	} else { \
		tu_fail_count++; \
		printf("FAIL %s: got %p, want %p [%s:%d]\n", \
		       (msg), tu_a_, tu_b_, __FILE__, __LINE__); \
	} \
} while (0)

#define TEST_SUMMARY(name) \
	printf("%s: %d passed, %d failed\n", (name), tu_pass_count, tu_fail_count)

/* ------------------------------------------------------------------ */
/* Shared helpers: temp paths, whole-file IO, directory creation/rm.   */
/* ------------------------------------------------------------------ */

/* Build a unique temp file path under ${TMPDIR:-/tmp} (mkstemp(3) is
 * race-free); the file exists and is empty when this returns 0.
 * Returns 0 on success, -1 on failure. */
TU_UNUSED
static int tu_temp_path(char *buf, size_t bufsz, const char *tag)
{
	const char *dir = getenv("TMPDIR");
	char tmpl[4096];
	int fd;

	if (!dir || !*dir)
		dir = "/tmp";
	if (snprintf(tmpl, sizeof(tmpl), "%s/lgmagic-%s-XXXXXX", dir, tag)
	    >= (int)sizeof(tmpl))
		return -1;
	fd = mkstemp(tmpl);
	if (fd < 0)
		return -1;
	close(fd);
	if (strlen(tmpl) + 1 > bufsz) {
		remove(tmpl);
		return -1;
	}
	strcpy(buf, tmpl);
	return 0;
}

/* Read an entire file into a malloc'd buffer (NUL-terminated).
 * Returns malloc'd buffer or NULL on error. */
TU_UNUSED
static char *tu_read_file(const char *path, size_t *len_out)
{
	FILE *f;
	char *buf = NULL;
	long sz;

	f = fopen(path, "rb");
	if (!f)
		return NULL;
	if (fseek(f, 0, SEEK_END) != 0) {
		fclose(f);
		return NULL;
	}
	sz = ftell(f);
	if (sz < 0 || fseek(f, 0, SEEK_SET) != 0) {
		fclose(f);
		return NULL;
	}
	buf = malloc((size_t)sz + 1);
	if (!buf) {
		fclose(f);
		return NULL;
	}
	if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
		free(buf);
		fclose(f);
		return NULL;
	}
	fclose(f);
	buf[sz] = '\0';
	if (len_out)
		*len_out = (size_t)sz;
	return buf;
}

/* Write raw bytes to a file (truncating); returns 0 or -1. */
TU_UNUSED
static int tu_write_file(const char *path, const void *data, size_t len)
{
	FILE *f = fopen(path, "wb");
	if (!f)
		return -1;
	if (fwrite(data, 1, len, f) != len) {
		fclose(f);
		return -1;
	}
	return fclose(f) == 0 ? 0 : -1;
}

/* mkdir -p for one path; returns 0 or -1. */
TU_UNUSED
static int tu_mkdir_p(const char *path)
{
	char tmp[4096];
	size_t n = strlen(path);
	size_t i;

	if (n >= sizeof(tmp))
		return -1;
	strcpy(tmp, path);
	if (n > 0 && tmp[n - 1] == '/')
		tmp[n - 1] = '\0';
	for (i = 1; tmp[i]; i++) {
		if (tmp[i] == '/') {
			tmp[i] = '\0';
			if (mkdir(tmp, 0755) != 0 && errno != EEXIST)
				return -1;
			tmp[i] = '/';
		}
	}
	if (mkdir(tmp, 0755) != 0 && errno != EEXIST)
		return -1;
	return 0;
}

/* Recursively remove a directory tree. Returns 0 or -1. */
TU_UNUSED
static int tu_rm_rf(const char *path)
{
	DIR *d;
	struct dirent *e;
	char child[4096];

	d = opendir(path);
	if (!d)
		return remove(path) == 0 ? 0 : -1;
	while ((e = readdir(d)) != NULL) {
		if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
			continue;
		if (snprintf(child, sizeof(child), "%s/%s", path, e->d_name)
		    >= (int)sizeof(child)) {
			closedir(d);
			return -1;
		}
		if (tu_rm_rf(child) != 0) {
			closedir(d);
			return -1;
		}
	}
	closedir(d);
	return rmdir(path) == 0 ? 0 : -1;
}

/* Resolve a fixture path: try dirs in order (cwd-relative); tests run with
 * cwd = tools/.  Returns 0 and fills buf, or -1 if not found anywhere. */
TU_UNUSED
static int tu_fixture_path(char *buf, size_t bufsz, const char *name)
{
	static const char *candidates[] = {
		"../testdata/",
		"testdata/",
		NULL
	};
	int i;
	FILE *f;

	for (i = 0; candidates[i]; i++) {
		if (snprintf(buf, bufsz, "%s%s", candidates[i], name)
		    >= (int)bufsz)
			return -1;
		f = fopen(buf, "rb");
		if (f) {
			fclose(f);
			return 0;
		}
	}
	return -1;
}

#endif /* LG_TOOLS_TESTS_TEST_UTIL_H */
