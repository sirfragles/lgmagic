/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * daemon_config.c - lgmagicd configuration.
 * See daemon_config.h for the layout and precedence.
 */
#include "daemon_config.h"

#include "toml.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

int daemon_config_init(struct daemon_config *dc, const char *config_root,
		       const char *state_dir, char *err, size_t errsz)
{
	memset(dc, 0, sizeof(*dc));
	snprintf(dc->config_root, sizeof(dc->config_root), "%s", config_root);
	snprintf(dc->state_dir, sizeof(dc->state_dir), "%s", state_dir);
	dc->global = config_load_daemon(config_root);
	if (!dc->global) {
		snprintf(err, errsz, "out of memory");
		return -1;
	}
	return 0;
}

void daemon_config_free(struct daemon_config *dc)
{
	config_free(dc->global);
	dc->global = NULL;
}

/* Join a + b into out (memmove: a may be out itself).  -1 (and out set
 * to "") when the result does not fit - the compiler cannot bound the
 * snprintf equivalents and warns. */
static int path_join(char *out, size_t outsz, const char *a, const char *b)
{
	size_t alen = strlen(a);
	size_t blen = strlen(b);

	if (alen + blen + 1 > outsz) {
		out[0] = '\0';
		return -1;
	}
	memmove(out, a, alen);
	memmove(out + alen, b, blen + 1);
	return 0;
}

int daemon_config_load_remote(const struct daemon_config *dc,
			      const char *identity, struct device_config *out,
			      char *err, size_t errsz)
{
	char path[4096];
	struct toml_value *root;
	const char *profile;
	int rc;

	device_config_init(out);
	if (path_join(path, sizeof(path), dc->config_root, "/devices.d/") < 0 ||
	    path_join(path, sizeof(path), path, identity) < 0 ||
	    path_join(path, sizeof(path), path, ".toml") < 0) {
		snprintf(err, errsz, "device config path too long");
		return -1;
	}
	rc = device_config_load_file(path, out, err, errsz);
	if (rc < 0)
		return -1;
	/* rc == 1: no device file - the defaults stay. */

	/* state.toml: only the active profile name of this device.  The
	 * two levels are resolved by hand (a dotted lookup would split the
	 * identity on '.'; MACs contain ':' but never '.'). */
	if (path_join(path, sizeof(path), dc->state_dir, "/state.toml") < 0) {
		snprintf(err, errsz, "state path too long");
		return -1;
	}
	root = toml_load_file(path, NULL, NULL);
	if (!root)
		return 0;	/* absent or unparseable - nothing to overlay */
	profile = NULL;
	{
		struct toml_value *devs = toml_table_get_short(root, "devices");
		struct toml_value *dev = devs && devs->type == TOML_TABLE ?
			toml_table_get_short(devs, identity) : NULL;

		if (dev)
			profile = toml_table_get_str(dev, "profile", NULL);
	}
	if (profile) {
		char *s = strdup(profile);

		if (!s) {
			toml_free(root);
			snprintf(err, errsz, "out of memory");
			return -1;
		}
		free(out->profile);
		out->profile = s;
	}
	toml_free(root);
	return 0;
}

/* ------------------------------------------------------------------ */
/* state.toml write                                                    */
/* ------------------------------------------------------------------ */

/* Set or replace a string member (toml has no remove API). */
static int table_set_str(struct toml_value *t, const char *key,
			 const char *val)
{
	struct toml_value *v = toml_new_str(val);
	size_t i;

	if (!v)
		return -1;
	for (i = 0; i < t->count; i++) {
		if (strcmp(t->keys[i], key) == 0) {
			toml_free(t->items[i]);
			t->items[i] = v;
			return 0;
		}
	}
	if (toml_table_add(t, key, v) < 0) {
		toml_free(v);
		return -1;
	}
	return 0;
}

static int mkdir_p(const char *dir, char *err, size_t errsz)
{
	char tmp[4096];
	char *p;
	size_t len;

	snprintf(tmp, sizeof(tmp), "%s", dir);
	len = strlen(tmp);
	if (len > 0 && tmp[len - 1] == '/')
		tmp[len - 1] = '\0';
	for (p = tmp + 1; *p; p++) {
		if (*p != '/')
			continue;
		*p = '\0';
		if (mkdir(tmp, 0755) < 0 && errno != EEXIST) {
			snprintf(err, errsz, "cannot create %s: %s", tmp,
				 strerror(errno));
			return -1;
		}
		*p = '/';
	}
	if (mkdir(tmp, 0755) < 0 && errno != EEXIST) {
		snprintf(err, errsz, "cannot create %s: %s", tmp,
			 strerror(errno));
		return -1;
	}
	return 0;
}

/* Atomic write: tmp + fsync + rename.  O_EXCL refuses to follow a
 * symlink planted at <path>.tmp - the state dir is daemon-owned, but
 * the write must never go through a link (symlink protection). */
static int atomic_write_file(const char *path, const char *text,
			     char *err, size_t errsz)
{
	char tmp[4096];
	size_t len = strlen(text);
	int fd;

	if (path_join(tmp, sizeof(tmp), path, ".tmp") < 0) {
		snprintf(err, errsz, "state file path too long");
		return -1;
	}
	fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_TRUNC, 0644);
	if (fd < 0 && errno == EEXIST) {
		/* a crashed write left a stale .tmp - remove and retry
		 * (unlink removes a planted symlink itself, not its target) */
		if (unlink(tmp) == 0)
			fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_TRUNC,
				  0644);
	}
	if (fd < 0) {
		snprintf(err, errsz, "cannot open %s: %s", tmp,
			 strerror(errno));
		return -1;
	}
	if (write(fd, text, len) != (ssize_t)len || fsync(fd) < 0) {
		snprintf(err, errsz, "write error on %s: %s", tmp,
			 strerror(errno));
		close(fd);
		unlink(tmp);
		return -1;
	}
	if (close(fd) < 0) {
		snprintf(err, errsz, "close error on %s: %s", tmp,
			 strerror(errno));
		unlink(tmp);
		return -1;
	}
	if (rename(tmp, path) < 0) {
		snprintf(err, errsz, "cannot rename %s to %s: %s", tmp, path,
			 strerror(errno));
		unlink(tmp);
		return -1;
	}
	return 0;
}

int daemon_config_save_profile(const struct daemon_config *dc,
			       const char *identity, const char *profile,
			       char *err, size_t errsz)
{
	char path[4096];
	struct toml_value *root, *devs, *dev;
	char *text;
	int rc;

	if (path_join(path, sizeof(path), dc->state_dir, "/state.toml") < 0) {
		snprintf(err, errsz, "state path too long");
		return -1;
	}
	root = toml_load_file(path, NULL, NULL);
	if (!root) {
		root = toml_new_table();
		if (!root)
			goto oom;
		{
			struct toml_value *ver = toml_new_int(2);

			if (!ver || toml_table_add(root, "version", ver) < 0) {
				toml_free(ver);
				goto oom_free_root;
			}
		}
	}

	devs = toml_table_get_short(root, "devices");
	if (!devs || devs->type != TOML_TABLE) {
		devs = toml_new_table();
		if (!devs || toml_table_add(root, "devices", devs) < 0) {
			toml_free(devs);
			goto oom_free_root;
		}
	}
	dev = toml_table_get_short(devs, identity);
	if (!dev || dev->type != TOML_TABLE) {
		dev = toml_new_table();
		if (!dev || toml_table_add(devs, identity, dev) < 0) {
			toml_free(dev);
			goto oom_free_root;
		}
	}
	if (table_set_str(dev, "profile", profile) < 0)
		goto oom_free_root;

	text = toml_dumps(root);
	toml_free(root);
	if (!text)
		goto oom;
	if (mkdir_p(dc->state_dir, err, errsz) < 0) {
		free(text);
		return -1;
	}
	rc = atomic_write_file(path, text, err, errsz);
	free(text);
	return rc;

oom_free_root:
	toml_free(root);
oom:
	snprintf(err, errsz, "out of memory");
	return -1;
}

int daemon_config_save_device(const struct daemon_config *dc,
			      const struct device_config *d,
			      const char *identity, char *err, size_t errsz)
{
	char path[4096], dir[4096];
	struct toml_value *root;
	char *text;
	int rc;

	if (path_join(path, sizeof(path), dc->config_root, "/devices.d/") < 0 ||
	    path_join(path, sizeof(path), path, identity) < 0 ||
	    path_join(path, sizeof(path), path, ".toml") < 0) {
		snprintf(err, errsz, "device config path too long");
		return -1;
	}
	root = device_config_to_toml(d);
	if (!root)
		goto oom;
	text = toml_dumps(root);
	toml_free(root);
	if (!text)
		goto oom;
	if (path_join(dir, sizeof(dir), dc->config_root, "/devices.d") < 0) {
		free(text);
		snprintf(err, errsz, "device config path too long");
		return -1;
	}
	if (mkdir_p(dir, err, errsz) < 0) {
		free(text);
		return -1;
	}
	rc = atomic_write_file(path, text, err, errsz);
	free(text);
	return rc;

oom:
	snprintf(err, errsz, "out of memory");
	return -1;
}

void daemon_config_calib_path(const struct daemon_config *dc,
			      const struct device_config *d,
			      const char *identity, char *out, size_t outsz)
{
	if (d->calib) {
		if (path_join(out, outsz, d->calib, "") < 0)
			out[0] = '\0';
	} else if (path_join(out, outsz, dc->state_dir, "/") < 0 ||
		   path_join(out, outsz, out, identity) < 0 ||
		   path_join(out, outsz, out, "/calibration.json") < 0) {
		out[0] = '\0';
	}
}
