/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * config.h - lgmagic user configuration.
 *
 * Precedence: built-in defaults < /etc/lgmagic/config.toml <
 * ~/.config/lgmagic/config.toml < --config FILE < CLI flags.
 * Files are TOML, parsed with our own toml.c (v1 JSON is converted with
 * `lgmagic config migrate`).
 */
#ifndef LG_TOOLS_CONFIG_H
#define LG_TOOLS_CONFIG_H

#include <stddef.h>

struct config {
	char *imu_device;		/* evdev path; NULL = auto-detect */
	char *hidraw_device;		/* hidraw path; NULL = auto-detect */
	char *default_calib;		/* calibration JSON path */
	double lpf_alpha;		/* mouse LPF gain (0.2) */
	double mouse_scale;		/* mouse movement multiplier (30.0) */
	double madgwick_beta;		/* AHRS gain (0.1) */
	double alpha;			/* kernel blob LPF (0.2) */
	double mouse_k;			/* kernel airmouse sensitivity (0.5) */
	double gyro_scale_default;	/* setup wizard default gyro scale (0.07) */
	int accel_gate;			/* spring-back gate on (1) */
	double accel_gate_lo;		/* gate opens above this accel dev (60.0) */
	double accel_gate_hi;		/* latch refresh above this accel dev (400.0) */
};

/* Load the effective configuration (see precedence above).
 * extra_path (--config FILE) is merged last; may be NULL. */
struct config *config_load(const char *extra_path);

/* The daemon's config: ONLY <config_root>/config.toml on top of the
 * defaults.  lgmagicd runs as root and never reads ~/.config (a root
 * process must not read user files). */
struct config *config_load_daemon(const char *config_root);

void config_free(struct config *cfg);

/* Save the effective config to ~/.config/lgmagic/config.toml. */
int config_save_user(struct config *cfg, char *err, size_t errsz);

/* Set one key by name (the names match the config file keys).
 * Returns 0 on success, -1 with err on unknown key / bad value. */
int config_set_key(struct config *cfg, const char *key, const char *value,
		   char *err, size_t errsz);

/* True if the key was explicitly set somewhere (file or flag), false if it
 * still has its built-in default. Used by the setup wizard to decide which
 * values to ask about. */
int config_is_explicit(const char *key);

/* Pretty-print the effective config and the config file paths. */
void config_print(const struct config *cfg);
void config_print_paths(void);

/* Global, set once by main(); all subcommands read it. */
extern struct config *g_cfg;
extern const char *g_tool_version;

#endif /* LG_TOOLS_CONFIG_H */
