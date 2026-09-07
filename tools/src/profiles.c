/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * profiles.c - per-device profiles and button maps (portable).
 * See profiles.h for the device TOML schema.
 */
#include "profiles.h"

#include "keymap.h"
#include "toml.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ------------------------------------------------------------------ */
/* Profile                                                             */
/* ------------------------------------------------------------------ */

int profile_init(struct profile *p, const char *name)
{
	memset(p, 0, sizeof(*p));
	p->name = strdup(name);
	if (!p->name)
		return -1;
	p->scroll_speed = 1.0;
	p->sensitivity = 30.0;
	p->lpf_alpha = -1.0;	/* unset: the daemon falls back to config */
	return 0;
}

void profile_free(struct profile *p)
{
	if (!p)
		return;
	free(p->name);
	free(p->map);
	memset(p, 0, sizeof(*p));
}

int profile_set_button(struct profile *p, int from, int to)
{
	size_t i;

	for (i = 0; i < p->nmap; i++) {
		if (p->map[i].from == from) {
			p->map[i].to = to;
			return 0;
		}
	}
	{
		struct button_map_entry *m = realloc(p->map,
			(p->nmap + 1) * sizeof(*m));

		if (!m)
			return -1;
		p->map = m;
		p->map[p->nmap].from = from;
		p->map[p->nmap].to = to;
		p->nmap++;
	}
	return 0;
}

void profile_clear_buttons(struct profile *p)
{
	free(p->map);
	p->map = NULL;
	p->nmap = 0;
}

int profile_validate(const struct profile *p, char *err, size_t errsz)
{
	size_t i;

	if (!p->name || !p->name[0]) {
		snprintf(err, errsz, "profile name is empty");
		return -1;
	}
	if (p->scroll_speed <= 0.0 || p->scroll_speed > 100.0) {
		snprintf(err, errsz, "profile '%s': scroll_speed %.6g out of "
			 "range (0..100]", p->name, p->scroll_speed);
		return -1;
	}
	if (p->sensitivity <= 0.0 || p->sensitivity > 10000.0) {
		snprintf(err, errsz, "profile '%s': sensitivity %.6g out of "
			 "range (0..10000]", p->name, p->sensitivity);
		return -1;
	}
	if (p->has_lpf && (p->lpf_alpha < 0.0 || p->lpf_alpha > 1.0)) {
		snprintf(err, errsz, "profile '%s': lpf_alpha %.6g out of "
			 "range (0..1)", p->name, p->lpf_alpha);
		return -1;
	}
	for (i = 0; i < p->nmap; i++) {
		if (!keymap_code_to_name(p->map[i].from)) {
			snprintf(err, errsz, "profile '%s': unknown keycode %d "
				 "in the button map", p->name, p->map[i].from);
			return -1;
		}
		if (!keymap_code_to_name(p->map[i].to)) {
			snprintf(err, errsz, "profile '%s': unknown keycode %d "
				 "in the button map", p->name, p->map[i].to);
			return -1;
		}
	}
	return 0;
}

int profile_merge(struct profile *dst, const struct profile *src)
{
	size_t i;

	if (src->has_scroll)
		dst->scroll_speed = src->scroll_speed;
	if (src->has_sens)
		dst->sensitivity = src->sensitivity;
	if (src->has_lpf)
		dst->lpf_alpha = src->lpf_alpha;
	for (i = 0; i < src->nmap; i++)
		if (profile_set_button(dst, src->map[i].from, src->map[i].to) < 0)
			return -1;
	return 0;
}

const struct profile *profile_find(const struct device_config *dc,
				   const char *name)
{
	size_t i;

	for (i = 0; i < dc->nprofiles; i++)
		if (strcmp(dc->profiles[i].name, name) == 0)
			return &dc->profiles[i];
	return NULL;
}

/* ------------------------------------------------------------------ */
/* Device config                                                       */
/* ------------------------------------------------------------------ */

void device_config_init(struct device_config *dc)
{
	memset(dc, 0, sizeof(*dc));
	dc->profile = strdup("default");
	dc->airmouse = 1;
}

void device_config_free(struct device_config *dc)
{
	size_t i;

	if (!dc)
		return;
	free(dc->profile);
	free(dc->calib);
	for (i = 0; i < dc->nprofiles; i++)
		profile_free(&dc->profiles[i]);
	free(dc->profiles);
	memset(dc, 0, sizeof(*dc));
}

/* Read a numeric field with strict typing.  Absent key: *out keeps its
 * current value and *has stays 0.  INT and FLOAT are both accepted. */
static int num_field(const struct toml_value *t, const char *key,
		     double *out, int *has, char *err, size_t errsz)
{
	const struct toml_value *v = toml_table_get_short(t, key);

	if (!v)
		return 0;
	if (v->type == TOML_FLOAT)
		*out = v->d;
	else if (v->type == TOML_INT)
		*out = (double)v->i;
	else {
		snprintf(err, errsz, "'%s' must be a number", key);
		return -1;
	}
	*has = 1;
	return 0;
}

/* Read a string field into *out (replacing the old value); the empty
 * string is preserved, the caller maps "" to "default" where needed. */
static int str_field(const struct toml_value *t, const char *key,
		     char **out, char *err, size_t errsz)
{
	const struct toml_value *v = toml_table_get_short(t, key);
	char *s;

	if (!v)
		return 0;
	if (v->type != TOML_STR) {
		snprintf(err, errsz, "'%s' must be a string", key);
		return -1;
	}
	s = strdup(v->str);
	if (!s) {
		snprintf(err, errsz, "out of memory");
		return -1;
	}
	free(*out);
	*out = s;
	return 0;
}

/* Fill the numeric fields and the button map of p from a profile table. */
static int profile_parse_fields(struct profile *p, const struct toml_value *t,
				char *err, size_t errsz)
{
	const struct toml_value *bmap;
	size_t i;

	if (num_field(t, "scroll_speed", &p->scroll_speed, &p->has_scroll,
		      err, errsz) < 0 ||
	    num_field(t, "sensitivity", &p->sensitivity, &p->has_sens,
		      err, errsz) < 0 ||
	    num_field(t, "lpf_alpha", &p->lpf_alpha, &p->has_lpf,
		      err, errsz) < 0)
		return -1;
	bmap = toml_table_get_short(t, "button_map");
	if (!bmap)
		return 0;
	if (bmap->type != TOML_TABLE) {
		snprintf(err, errsz, "'button_map' must be a table");
		return -1;
	}
	for (i = 0; i < bmap->count; i++) {
		struct toml_value *v = bmap->items[i];
		int from, to;

		if (v->type != TOML_STR) {
			snprintf(err, errsz, "button_map values must be "
				 "strings");
			return -1;
		}
		from = keymap_name_to_code(bmap->keys[i]);
		if (from < 0) {
			snprintf(err, errsz, "unknown keycode '%s' in the "
				 "button map", bmap->keys[i]);
			return -1;
		}
		to = keymap_name_to_code(v->str);
		if (to < 0) {
			snprintf(err, errsz, "unknown keycode '%s' in the "
				 "button map", v->str);
			return -1;
		}
		if (profile_set_button(p, from, to) < 0) {
			snprintf(err, errsz, "out of memory");
			return -1;
		}
	}
	return 0;
}

int device_config_from_toml(const struct toml_value *root,
			    struct device_config *dc, char *err, size_t errsz)
{
	const struct toml_value *profiles;
	size_t i;

	if (!root || root->type != TOML_TABLE) {
		snprintf(err, errsz, "device config root is not a table");
		return -1;
	}
	/* Profiles are parsed from scratch; the scalar fields keep the
	 * caller's defaults when their keys are absent. */
	for (i = 0; i < dc->nprofiles; i++)
		profile_free(&dc->profiles[i]);
	free(dc->profiles);
	dc->profiles = NULL;
	dc->nprofiles = 0;

	if (str_field(root, "profile", &dc->profile, err, errsz) < 0 ||
	    str_field(root, "calib", &dc->calib, err, errsz) < 0)
		return -1;
	if (dc->calib && !dc->calib[0]) {
		free(dc->calib);	/* "" = use the default calibration */
		dc->calib = NULL;
	}
	{
		const struct toml_value *v = toml_table_get_short(root, "airmouse");

		if (v) {
			if (v->type != TOML_BOOL) {
				snprintf(err, errsz, "'airmouse' must be a "
					 "boolean");
				return -1;
			}
			dc->airmouse = v->b;
			dc->has_airmouse = 1;
		}
	}
	profiles = toml_table_get_short(root, "profiles");
	if (profiles) {
		if (profiles->type != TOML_TABLE) {
			snprintf(err, errsz, "'profiles' must be a table");
			return -1;
		}
		for (i = 0; i < profiles->count; i++) {
			struct profile p;

			if (profiles->items[i]->type != TOML_TABLE) {
				snprintf(err, errsz, "profile '%s' must be a "
					 "table", profiles->keys[i]);
				return -1;
			}
			if (profile_find(dc, profiles->keys[i])) {
				snprintf(err, errsz, "duplicate profile '%s'",
					 profiles->keys[i]);
				return -1;
			}
			if (profile_init(&p, profiles->keys[i]) < 0) {
				snprintf(err, errsz, "out of memory");
				return -1;
			}
			if (profile_parse_fields(&p, profiles->items[i], err,
						 errsz) < 0 ||
			    profile_validate(&p, err, errsz) < 0) {
				profile_free(&p);
				return -1;
			}
			{
				struct profile *arr = realloc(dc->profiles,
					(dc->nprofiles + 1) * sizeof(*arr));

				if (!arr) {
					snprintf(err, errsz, "out of memory");
					profile_free(&p);
					return -1;
				}
				dc->profiles = arr;
				dc->profiles[dc->nprofiles++] = p;
			}
		}
	}
	return device_config_validate(dc, err, errsz);
}

int device_config_validate(const struct device_config *dc, char *err,
			   size_t errsz)
{
	size_t i;

	if (!dc->profile || !dc->profile[0]) {
		snprintf(err, errsz, "active profile name is empty");
		return -1;
	}
	for (i = 0; i < dc->nprofiles; i++)
		if (profile_validate(&dc->profiles[i], err, errsz) < 0)
			return -1;
	/* The active profile may be the implicit built-in "default". */
	if (strcmp(dc->profile, "default") != 0 &&
	    !profile_find(dc, dc->profile)) {
		snprintf(err, errsz, "active profile '%s' is not defined",
			 dc->profile);
		return -1;
	}
	return 0;
}

int device_config_load_file(const char *path, struct device_config *dc,
			    char *err, size_t errsz)
{
	const char *terr = NULL;
	size_t line = 0;
	struct toml_value *root;

	if (access(path, F_OK) != 0) {
		if (errno == ENOENT)
			return 1;	/* no override file: keep defaults */
		snprintf(err, errsz, "%s: %s", path, strerror(errno));
		return -1;
	}
	root = toml_load_file(path, &terr, &line);
	if (!root) {
		snprintf(err, errsz, "%s: %s (line %zu)", path,
			 terr ? terr : "parse error", line);
		return -1;
	}
	if (device_config_from_toml(root, dc, err, errsz) < 0) {
		toml_free(root);
		return -1;
	}
	toml_free(root);
	return 0;
}

/* ------------------------------------------------------------------ */
/* Bus-side mutation helpers                                           */
/* ------------------------------------------------------------------ */

struct profile *device_config_profile_ensure(struct device_config *dc,
					     const char *name)
{
	struct profile *arr, *p;
	size_t i;

	for (i = 0; i < dc->nprofiles; i++)
		if (strcmp(dc->profiles[i].name, name) == 0)
			return &dc->profiles[i];
	arr = realloc(dc->profiles, (dc->nprofiles + 1) * sizeof(*arr));
	if (!arr)
		return NULL;
	dc->profiles = arr;
	p = &dc->profiles[dc->nprofiles];
	if (profile_init(p, name) < 0)
		return NULL;	/* zeroed - safe for device_config_free */
	dc->nprofiles++;
	return p;
}

struct toml_value *device_config_to_toml(const struct device_config *dc)
{
	struct toml_value *root, *profiles;
	size_t i;

	root = toml_new_table();
	if (!root)
		return NULL;
	if (toml_table_add(root, "profile", toml_new_str(dc->profile)) < 0)
		goto oom;
	if (dc->calib &&
	    toml_table_add(root, "calib", toml_new_str(dc->calib)) < 0)
		goto oom;
	if (dc->has_airmouse &&
	    toml_table_add(root, "airmouse", toml_new_bool(dc->airmouse)) < 0)
		goto oom;
	if (dc->nprofiles == 0)
		return root;

	profiles = toml_new_table();
	if (!profiles || toml_table_add(root, "profiles", profiles) < 0) {
		toml_free(profiles);
		goto oom;
	}
	for (i = 0; i < dc->nprofiles; i++) {
		const struct profile *p = &dc->profiles[i];
		struct toml_value *t = toml_new_table();
		size_t k;

		if (!t || toml_table_add(profiles, p->name, t) < 0) {
			toml_free(t);
			goto oom;
		}
		if (p->has_scroll &&
		    toml_table_add(t, "scroll_speed",
				   toml_new_float(p->scroll_speed)) < 0)
			goto oom;
		if (p->has_sens &&
		    toml_table_add(t, "sensitivity",
				   toml_new_float(p->sensitivity)) < 0)
			goto oom;
		if (p->has_lpf &&
		    toml_table_add(t, "lpf_alpha",
				   toml_new_float(p->lpf_alpha)) < 0)
			goto oom;
		if (p->nmap > 0) {
			struct toml_value *bm = toml_new_table();

			if (!bm || toml_table_add(t, "button_map", bm) < 0) {
				toml_free(bm);
				goto oom;
			}
			for (k = 0; k < p->nmap; k++) {
				const char *from = keymap_code_to_name(
					p->map[k].from);
				const char *to = keymap_code_to_name(p->map[k].to);

				/* validation guarantees known keycodes */
				if (!from || !to ||
				    toml_table_add(bm, from, toml_new_str(to)) < 0)
					goto oom;
			}
		}
	}
	return root;

oom:
	toml_free(root);
	return NULL;
}
