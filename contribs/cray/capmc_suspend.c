/*****************************************************************************\
 *  capmc_suspend.c - Power down identified nodes
 *
 *  Usage: "capmc_suspend <hostlist>"
 *****************************************************************************
 *  Copyright (C) 2016 SchedMD LLC.
 *
 *  This file is part of SLURM, a resource management program.
 *  For details, see <http://slurm.schedmd.com/>.
 *  Please also read the included file: DISCLAIMER.
 *
 *  SLURM is free software; you can redistribute it and/or modify it under
 *  the terms of the GNU General Public License as published by the Free
 *  Software Foundation; either version 2 of the License, or (at your option)
 *  any later version.
 *
 *  In addition, as a special exception, the copyright holders give permission
 *  to link the code of portions of this program with the OpenSSL library under
 *  certain conditions as described in each individual source file, and
 *  distribute linked combinations including the two. You must obey the GNU
 *  General Public License in all respects for all of the code used other than
 *  OpenSSL. If you modify file(s) with this exception, you may extend this
 *  exception to your version of the file(s), but you are not obligated to do
 *  so. If you do not wish to do so, delete this exception statement from your
 *  version.  If you delete this exception statement from all source files in
 *  the program, then also delete it here.
 *
 *  SLURM is distributed in the hope that it will be useful, but WITHOUT ANY
 *  WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 *  FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 *  details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with SLURM; if not, write to the Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA.
\*****************************************************************************/

#if HAVE_CONFIG_H
#  include "config.h"
#endif

#define _GNU_SOURCE	/* For POLLRDHUP */

#if HAVE_JSON_C_INC
#  include <json-c/json.h>
#elif HAVE_JSON_INC
#  include <json/json.h>
#endif

#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "slurm/slurm.h"
#include "slurm/slurm_errno.h"
#include "src/common/hostlist.h"
#include "src/common/log.h"
#include "src/common/macros.h"
#include "src/common/parse_config.h"
#include "src/common/read_config.h"
#include "src/common/slurm_protocol_api.h"
#include "src/common/xassert.h"
#include "src/common/xmalloc.h"
#include "src/common/xstring.h"

#define MAX_THREADS 256

/* Maximum poll wait time for child processes, in milliseconds */
#define MAX_POLL_WAIT 500

#define DEFAULT_CAPMC_TIMEOUT 10000	/* 10 seconds */
#define MIN_CAPMC_TIMEOUT 1000		/* 1 second */

/* Number of times to try performing "node_off" operation */
#define NODE_OFF_RETRIES 10

/* How long to wait for a node to enter "off" state, in seconds */
#define NODE_OFF_STATE_WAIT (30 * 60)

/* Static variables */
static char *capmc_path = NULL;
static uint32_t capmc_poll_freq = 45;   /* capmc state polling frequency */
static uint32_t capmc_timeout = DEFAULT_CAPMC_TIMEOUT;
static char *log_file = NULL;
static char *prog_name = NULL;

static pthread_mutex_t thread_cnt_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  thread_cnt_cond  = PTHREAD_COND_INITIALIZER;
static int thread_cnt = 0;

/* NOTE: Keep this table synchronized with the table in
 * src/plugins/node_features/knl_cray/node_features_knl_cray.c */
static s_p_options_t knl_conf_file_options[] = {
	{"AllowMCDRAM", S_P_STRING},
	{"AllowNUMA", S_P_STRING},
	{"AllowUserBoot", S_P_STRING},
	{"CapmcPath", S_P_STRING},
	{"CapmcPollFreq", S_P_UINT32},
	{"CapmcTimeout", S_P_UINT32},
	{"DefaultMCDRAM", S_P_STRING},
	{"DefaultNUMA", S_P_STRING},
	{"LogFile", S_P_STRING},
	{"SyscfgPath", S_P_STRING},
	{NULL}
};

static bool _check_node_state(int nid, char *nid_str, char *state);
static s_p_hashtbl_t *_config_make_tbl(char *filename);
static uint32_t *_json_parse_nids(json_object *jobj, char *key, int *num);
static void *_node_update(void *args);
static void _read_config(void);
static char *_run_script(char **script_argv, int *status);
static int _tot_wait(struct timeval *start_time);

static s_p_hashtbl_t *_config_make_tbl(char *filename)
{
	s_p_hashtbl_t *tbl = NULL;

	xassert(filename);

	if (!(tbl = s_p_hashtbl_create(knl_conf_file_options))) {
		error("%s: s_p_hashtbl_create error: %s", prog_name,
		      slurm_strerror(slurm_get_errno()));
		return tbl;
	}

	if (s_p_parse_file(tbl, NULL, filename, false) == SLURM_ERROR) {
		error("%s: s_p_parse_file error: %s", prog_name,
		      slurm_strerror(slurm_get_errno()));
		s_p_hashtbl_destroy(tbl);
		tbl = NULL;
	}

	return tbl;
}

static void _read_config(void)
{
	char *knl_conf_file;
	s_p_hashtbl_t *tbl;

	capmc_timeout = DEFAULT_CAPMC_TIMEOUT;
	knl_conf_file = get_extra_conf_path("knl_cray.conf");
	if ((tbl = _config_make_tbl(knl_conf_file))) {
		(void) s_p_get_string(&capmc_path, "CapmcPath", tbl);
		(void) s_p_get_uint32(&capmc_poll_freq, "CapmcPollFreq", tbl);
		(void) s_p_get_uint32(&capmc_timeout, "CapmcTimeout", tbl);
		(void) s_p_get_string(&log_file, "LogFile", tbl);
	}
	xfree(knl_conf_file);
	s_p_hashtbl_destroy(tbl);
	if (!capmc_path)
		capmc_path = xstrdup("/opt/cray/capmc/default/bin/capmc");
	capmc_timeout = MAX(capmc_timeout, MIN_CAPMC_TIMEOUT);
	if (!log_file)
		log_file = slurm_get_job_slurmctld_logfile();
}

/*
 * Return time in msec since "start time"
 */
static int _tot_wait(struct timeval *start_time)
{
	struct timeval end_time;
	int msec_delay;

	gettimeofday(&end_time, NULL);
	msec_delay =   (end_time.tv_sec  - start_time->tv_sec ) * 1000;
	msec_delay += ((end_time.tv_usec - start_time->tv_usec + 500) / 1000);
	return msec_delay;
}

/* Run a script and return its stdout plus exit status */
static char *_run_script(char **script_argv, int *status)
{
	int cc, i, new_wait, resp_size = 0, resp_offset = 0;
	pid_t cpid;
	char *resp = NULL;
	int pfd[2] = { -1, -1 };

	if (access(capmc_path, R_OK | X_OK) < 0) {
		error("%s: Can not execute: %s", prog_name, capmc_path);
		*status = 127;
		resp = xstrdup("Slurm node_features/knl_cray configuration error");
		return resp;
	}
	if (pipe(pfd) != 0) {
		error("%s: pipe(): %s", prog_name,
		      slurm_strerror(slurm_get_errno()));
		*status = 127;
		resp = xstrdup("System error");
		return resp;
	}

	if ((cpid = fork()) == 0) {
		cc = sysconf(_SC_OPEN_MAX);
		dup2(pfd[1], STDERR_FILENO);
		dup2(pfd[1], STDOUT_FILENO);
		for (i = 0; i < cc; i++) {
			if ((i != STDERR_FILENO) && (i != STDOUT_FILENO))
				close(i);
		}
		setpgid(0, 0);
		execv(capmc_path, script_argv);
		error("%s: execv(): %s", prog_name,
		      slurm_strerror(slurm_get_errno()));
		exit(127);
	} else if (cpid < 0) {
		close(pfd[0]);
		close(pfd[1]);
		error("%s: fork(): %s", prog_name,
		      slurm_strerror(slurm_get_errno()));
		*status = 127;
		resp = xstrdup("System error");
		return resp;
	} else {
		struct pollfd fds;
		struct timeval tstart;
		resp_size = 1024;
		resp = xmalloc(resp_size);
		close(pfd[1]);
		gettimeofday(&tstart, NULL);
		while (1) {
			fds.fd = pfd[0];
			fds.events = POLLIN | POLLHUP | POLLRDHUP;
			fds.revents = 0;
			new_wait = capmc_timeout - _tot_wait(&tstart);
			if (new_wait <= 0) {
				error("%s: poll() timeout @ %d msec", prog_name,
				      capmc_timeout);
				break;
			}
			new_wait = MIN(new_wait, MAX_POLL_WAIT);
			i = poll(&fds, 1, new_wait);
			if (i == 0) {
				continue;
			} else if (i < 0) {
				error("%s: poll(): %s", prog_name,
				      slurm_strerror(slurm_get_errno()));
				break;
			}
			if ((fds.revents & POLLIN) == 0)
				break;
			i = read(pfd[0], resp + resp_offset,
				 resp_size - resp_offset);
			if (i == 0) {
				break;
			} else if (i < 0) {
				if (errno == EAGAIN)
					continue;
				error("%s: read(): %s", prog_name,
				      slurm_strerror(slurm_get_errno()));
				break;
			} else {
				resp_offset += i;
				if (resp_offset + 1024 >= resp_size) {
					resp_size *= 2;
					resp = xrealloc(resp, resp_size);
				}
			}
		}
		killpg(cpid, SIGTERM);
		usleep(10000);
		killpg(cpid, SIGKILL);
		waitpid(cpid, status, 0);
		close(pfd[0]);
	}
	return resp;
}

static uint32_t *_json_parse_nids(json_object *jobj, char *key, int *num)
{
	json_object *j_array = NULL;
	json_object *j_value = NULL;
	enum json_type j_type;
	uint32_t *ents;
	int i, cnt;

	*num = 0;
        json_object_object_get_ex(jobj, key, &j_array);
	if (!j_array) {
		debug("%s: key=%s not found in nid specification",
		      prog_name, key);
		return NULL;
	}

	cnt = json_object_array_length(j_array);
	ents = xmalloc(sizeof(uint32_t) * cnt);
	for (i = 0; i < cnt; i++) {
		j_value = json_object_array_get_idx(j_array, i);
		j_type = json_object_get_type(j_value);
		if (j_type != json_type_int) {
			error("%s: Unable to parse nid specification",
			      prog_name);
			break;
		} else {
			ents[i] = (uint32_t) json_object_get_int64(j_value);
			*num = i + 1;
		}
	}
	return ents;
}

static bool _check_node_state(int nid, char *nid_str, char *state)
{
	bool node_state_ok = false;
	char *argv[10], *resp_msg;
	int i, nid_cnt, status = 0;
	uint32_t *nid_array;
	json_object *j;

	argv[0] = "capmc";
	argv[1] = "node_status";
	argv[2] = "-n";
	argv[3] = nid_str;
	argv[4] = NULL;
	resp_msg = _run_script(argv, &status);
	if (status != 0) {
		error("%s: capmc(%s,%s,%s): %d %s", prog_name,
			argv[1], argv[2], argv[3], status, resp_msg);
		xfree(resp_msg);
		return node_state_ok;
	}
	j = json_tokener_parse(resp_msg);
	if (j == NULL) {
		error("%s: json parser failed on %s", prog_name, resp_msg);
		xfree(resp_msg);
		return node_state_ok;
	}
	xfree(resp_msg);

	nid_cnt = 0;
	nid_array = _json_parse_nids(j, "off", &nid_cnt);
	json_object_put(j);	/* Frees json memory */
	for (i = 0; i < nid_cnt; i++) {
		if (nid_array[i] == nid) {
			node_state_ok = true;
			break;
		}
	}
	xfree(nid_array);

	return node_state_ok;
}

static void *_node_update(void *args)
{
	char *node_name = (char *) args;
	char *argv[10], nid_str[32], *resp_msg;
	int i, nid = -1, status = 0;
	bool node_state_ok, node_off_sent = false;
	time_t poll_start;

	for (i = 0; node_name[i]; i++) {
		if ((node_name[i] >= '0') && (node_name[i] <= '9')) {
			nid = strtol(node_name + i, NULL, 10);
			break;
		}
	}
	if (nid < 0) {
		error("%s: No valid NID: %s", prog_name, node_name);
		return NULL;
	}
	snprintf(nid_str, sizeof(nid_str), "%d", nid);

	/* Request node power down.
	 * Example: "capmc node_off –n 43" */
	argv[0] = "capmc";
	argv[1] = "node_off";
	argv[2] = "-n";
	argv[3] = nid_str;
	argv[4] = NULL;
	for (i = 0; ((i < NODE_OFF_RETRIES) && !node_off_sent); i++) {
		resp_msg = _run_script(argv, &status);
		if ((status != 0) ||
		    (resp_msg && (strcasestr(resp_msg, "Success") == NULL))) {
			error("%s: capmc(%s,%s,%s): %d %s", prog_name,
			      argv[1], argv[2], argv[3], status, resp_msg);
			sleep(1);
		} else {
			debug("%s: node_off sent to %s", prog_name, nid_str);
			node_off_sent = true;
		}
		xfree(resp_msg);
	}

	/* Wait for node in "off" state */
	poll_start = time(NULL);
	while (!node_state_ok &&
	      (difftime(time(NULL), poll_start) < NODE_OFF_STATE_WAIT)) {
		sleep(capmc_poll_freq);
		node_state_ok = _check_node_state(nid, nid_str, "off");
	}

	slurm_mutex_lock(&thread_cnt_mutex);
	thread_cnt--;
	pthread_cond_signal(&thread_cnt_cond);
	slurm_mutex_unlock(&thread_cnt_mutex);
	return NULL;
}

int main(int argc, char *argv[])
{
	log_options_t log_opts = LOG_OPTS_INITIALIZER;
	hostlist_t hl = NULL;
	char *node_name;
	pthread_attr_t attr_work;
	pthread_t thread_work = 0;

	xstrfmtcat(prog_name, "%s[%u]", argv[0], (uint32_t) getpid());
	_read_config();
	log_opts.stderr_level = LOG_LEVEL_QUIET;
	log_opts.syslog_level = LOG_LEVEL_QUIET;
	if (slurm_get_debug_flags() && DEBUG_FLAG_NODE_FEATURES)
		log_opts.logfile_level += 3;
	(void) log_init(argv[0], log_opts, LOG_DAEMON, log_file);

	if ((hl = hostlist_create(argv[1])) == NULL) {
		error("%s: Invalid hostlist (%s)", prog_name, argv[1]);
		exit(2);
	}
	while ((node_name = hostlist_pop(hl))) {
		slurm_mutex_lock(&thread_cnt_mutex);
		while (1) {
			if (thread_cnt <= MAX_THREADS) {
				thread_cnt++;
				break;
			} else {	/* wait for state change and retry */
				pthread_cond_wait(&thread_cnt_cond,
						  &thread_cnt_mutex);
			}
		}
		slurm_mutex_unlock(&thread_cnt_mutex);

		slurm_attr_init(&attr_work);
		(void) pthread_attr_setdetachstate
			(&attr_work, PTHREAD_CREATE_DETACHED);
		if (pthread_create(&thread_work, &attr_work, _node_update,
				   (void *) node_name)) {
			_node_update((void *) node_name);
		}
		slurm_attr_destroy(&attr_work);
	}

	/* Wait for work threads to complete */
	slurm_mutex_lock(&thread_cnt_mutex);
	while (1) {
		if (thread_cnt == 0)
			break;
		else	/* wait for state change and retry */
			pthread_cond_wait(&thread_cnt_cond, &thread_cnt_mutex);
	}
	slurm_mutex_unlock(&thread_cnt_mutex);
	hostlist_destroy(hl);

	exit(0);
}
