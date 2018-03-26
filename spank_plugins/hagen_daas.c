/*
 *   Howto Avoid Grabbing Emulators Nightlong: DLS as a Service
 *
 *   To compile:
 *    gcc -shared -o hmaas_opts.so hmaas_opts.c
 *
 */
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/resource.h>
#include <sys/types.h>
#include <pwd.h>

#include <slurm/spank.h>
#include <slurm/slurm.h>

#include "src/common/env.h"
#include "src/common/hagen_daas.h"
#include "src/common/slurm_protocol_defs.h"

/*
 * All spank plugins must define this macro for the
 * Slurm plugin loader.
 *
 * Supported spank options are job-id prefix;
 */
SPANK_PLUGIN(hagen_daas, 1);

static int _check_opt(int val, const char* optarg, int remote);
struct spank_option my_spank_options[] = {
	{	"daas-board-id", "[board-id]",
		"Board id (currently USB serial, same as gres) of the hardware board to connect to.",
		1, 0, (spank_opt_cb_f) _check_opt},
	{	"launch-scoop", "[board-id]",
		"Launch a scoop (arbiter daemon) for the given board id.",
		1, 0, (spank_opt_cb_f) _check_opt},
	SPANK_OPTIONS_TABLE_END};

/********************************
 * helper function declarations *
 *******************************/

/* Check if the given job uses hagen daas
 */
static bool _check_job_use_hagen_daas(spank_t sp);


/* launch a job that would start a scoop
 * -> if the scoop job gets queued automatically, wait for it to run
 *    -> if it does not run immediately, update dependency of current job and reqeue
 *
 * (race-condition)
 * If the scoop job fails to queue, then another compute job already queued the job in the meantime (i.e. the time
 * between job_submit_hagen_daas queuing this job and the time it took to execute it). We hence have to do a "costly"
 * search by jobname and running user to identify the scoop job (since board-ids are unique the jobname should also be
 * unique) and see if it is running. If not, also wait for it and requeue.
 */
static int _queue_scoop_job(spank_t sp);


/* Make sure the scoop job is running.
 *
 * Schedule a scoop and reschedule the current job if necessary.
 */
static int _ensure_scoop_running(spank_t sp);


/* init and set environment for scoop job
 */
static int _set_env_scoop(job_desc_msg_t* job_desc, char const* board_id);


/* Add job_id as running dependency to current job and then have the scheduler requeue us.
 */
static int _wait_for_job_id(spank_t sp, uint32_t job_id);


/* Check if the job with the given job id is actually running (return value 0) or pending (return value 1).
 * In any other case (job terminated or general failure) the return value is -1.
 */
static int _check_job_running(uint32_t job_id);

/* Check if the job with the given job id is actually running (return value 0) or pending (return value 1).
 * In any other case (job terminated or general failure) the return value is -1.
 *
 * In addition to _check_job_running, send SIGCONT prior to checking the job status, causing quiggeldy
 * to reset its timeout counter.
 */
static int _check_scoop_job_running(uint32_t job_id);

/* Find job_id to scoop job (running or pending) for the given board id.
 *
 * Returns 0 if the job_id was found, -1 otherwise.
 */
static int _board_id_to_scoop_job_id(char const* board_id, uint32_t* job_id);

/* Find an existing scoop job by board_id and wait for it if it is pending.
 *
 * Return 0 if scoop job id successfully found, -1 otherwise (e.g. scoop job not existing).
 */
static int _find_wait_existing_scoop_job(spank_t sp);

/************************
 * official plugin api  *
 ************************/

int slurm_spank_init(spank_t sp, int ac, char** av)
{
	size_t optioncounter;
	struct spank_option endmarker = SPANK_OPTIONS_TABLE_END;
	for (optioncounter = 0; my_spank_options[optioncounter].name != endmarker.name;
		 optioncounter++) {
		if (spank_option_register(sp, &my_spank_options[optioncounter]) != ESPANK_SUCCESS)
		{
			return -1;
		}
	}

	// we only want to continue in remote context, i.e. when the job is about to run
	switch(spank_context())
	{
		case S_CTX_REMOTE:
			break;
		default:
			return 0;
	}
	if (!_check_job_use_hagen_daas(sp))
	{
		slurm_debug("[hagen-daas] No magic hagen-daas magic cookie found!");
		// nothing to do for jobs that do not use hagen daas
		return 0;
	}
	return _ensure_scoop_running(sp);
}


/*******************************
 * Helper function definitions *
 ******************************/

static int _check_opt(int val, const char* optarg, int remote)
{
	if (optarg == NULL)
		return -1;
	return 0;
}

static bool _check_job_use_hagen_daas(spank_t sp)
{
	char buffer[MAX_LENGTH_ARGUMENT];
	memset(buffer, 0, MAX_LENGTH_ARGUMENT);
	spank_err_t rc;

	// we just check the board id because it is always set
	if (spank_getenv(sp, hd_env_name_magic, buffer, MAX_LENGTH_ARGUMENT) == ESPANK_SUCCESS)
	{
		return strcmp(hd_env_content_magic, buffer) == 0;
	}
	else
	{
		return false;
	}
}

static int _ensure_scoop_running(spank_t sp)
{
	// check if env varibale of already running scoop job id is set
	// -> if so, check if that job is still running
	// -> (TODO: send ping)

	char job_id_str[MAX_LENGTH_ARGUMENT];
	int rc;

	rc = spank_getenv(sp, hd_env_name_scoop_job_id, job_id_str, MAX_LENGTH_ARGUMENT);

	if (rc == ESPANK_SUCCESS)
	{
		slurm_debug("[hagen-daas] Read scoop job id: %s", job_id_str);
		int job_id_int;
		job_id_int = atoi(job_id_str);
		rc = _check_scoop_job_running(job_id_int);
		if (rc > 0)
		{
			// job is pending
			return _wait_for_job_id(sp, job_id_int);
		}
		else if (rc == 0){
			// job exists and is running -> we are done
			return 0;
		}
		else
		{
			// job is already terminated -> start anew
			return _queue_scoop_job(sp);
		}
	}
	else if (rc != ESPANK_ENV_NOEXIST)
	{
		slurm_error("[hagen-daas] There was an error retreving %s from environment", hd_env_name_scoop_job_id);
		// something went wrong
		return -1;
	}
	else
	{
		// env with scoop job id was not set
		// -> try to find scoop job manually first, then queue a possible scoop job
		if (_find_wait_existing_scoop_job(sp) != 0)
		{
			return _queue_scoop_job(sp);
		}
		else
		{
			return 0;
		}

	}
}

static int _queue_scoop_job(spank_t sp)
{
	char board_id[MAX_LENGTH_ARGUMENT];
	uint32_t job_id = 0;

	uint32_t local_uid;
	uint32_t local_gid;

	int rc;

	if (spank_getenv(sp, hd_env_name_scoop_board_id, board_id, MAX_LENGTH_ARGUMENT) != ESPANK_SUCCESS)
	{
		return -1;
	}
	job_desc_msg_t job_desc;
	submit_response_msg_t* resp = NULL;

	slurm_init_job_desc_msg(&job_desc);

	job_desc.script = "#!/bin/sh\n\n#this is a dummy script\nexit -1\n";

	if ((spank_get_item(sp, S_JOB_UID, &local_uid) != 0) || (spank_get_item(sp, S_JOB_GID, &local_gid) != 0))
	{
		slurm_error("[hagen-daas] Could not get UID/GID of compute job.");
		return -1;
	}

	job_desc.user_id = local_uid;
	job_desc.group_id = local_gid;

	if (_set_env_scoop(&job_desc, board_id) != 0)
	{
		return -1;
	}

	slurm_debug("[hagen-daas] job_desc->user_id: %d", job_desc.user_id);
	slurm_debug("[hagen-daas] job_desc->group_id: %d", job_desc.group_id);
	slurm_debug("[hagen-daas] job_desc->script: %s", job_desc.script);

	slurm_info("[hagen-daas] Submitting scoop job.");
	rc = slurm_submit_batch_job(&job_desc, &resp);

	if (rc == SLURM_SUCCESS)
	{
		slurm_debug("[hagen-daas] Received error code: %d", resp->error_code);
		slurm_debug("[hagen-daas] Scoop running in job #%d", resp->job_id);

		job_id = resp->job_id;
		slurm_free_submit_response_response_msg(resp);

		// job was successfully scheduled, which means that the scoop was not running prior
		// -> we have to wait for it.
		// Wait for a second and check if it started running, if not, requeue us so that we only run after the scoop has
		// been started
		slurm_info("[hagen-daas] Sleeping to wait for scoop launch!");
		sleep(hd_scoop_launch_wait_secs); // wait for scoop to start
		rc = _check_scoop_job_running(job_id);

		slurm_debug("[hagen-daas] RC for _check_scoop_job_running: %d", rc);

		if (rc == 0)
		{
			// scoop launched successful
			return 0;
		}
		else if (rc > 0)
		{
			// scoop queued and not started immedately -> wait
			return _wait_for_job_id(sp, job_id);
		}
		else
		{
			// something went terribly wrong
			slurm_error("[hagen-daas] Could not launch scoop!");
			return -1;
		}
	}
	else
	{
		slurm_debug("[hagen-daas] Received errno: %d", errno);
		slurm_debug("[hagen-daas] Response message: %p", resp);

		// note that resp == NULL here -> no freeing needed!

		// in order to avoid race condition, wait for scoop started by another job to start
		size_t i;
		for (i=0; i < hd_scoop_launch_wait_num_periods; ++i)
		{
			slurm_info("[hagen-daas] Waiting for hardware control daemon job to start.. Elapsed: %zus / Max: %ds",
					i * hd_scoop_launch_wait_period_secs,
					hd_scoop_launch_wait_num_periods * hd_scoop_launch_wait_period_secs);

			sleep(hd_scoop_launch_wait_period_secs);

			if (job_id == 0)
			{
				_board_id_to_scoop_job_id(board_id, &job_id);
			}
			if (job_id > 0 && _check_scoop_job_running(job_id) == 0)
			{
				// we wait for the job to be actually started
				break;
			}
		}

		// scoop job failed to start -> scoop is probably already running, but we still need to make sure
		if (job_id == 0 && _board_id_to_scoop_job_id(board_id, &job_id) != 0)
		{
			slurm_error("[hagen-daas] Scoop job should have been launched but now there is not trace of it!");
			return -1;
		}

		rc = _check_scoop_job_running(job_id);
		if (rc == 0)
		{
			// scoop is running -> nothing to do
			return 0;
		}
		else if (rc == 1)
		{
			// scoop was launched but is pending -> wait for it
			return _wait_for_job_id(sp, job_id);
		}
		else
		{
			slurm_error("[hagen-daas] Scoop appears to have crashed and burnt!");
			return -1;
		}
	}
}

static int _set_env_scoop(job_desc_msg_t* job_desc, char const* board_id)
{
	// environment needs to exist! otherwise job launch will fail
	// environment will be overwritten however, so a dummy var/value is enough
	job_desc->environment = env_array_create();
	if (env_array_append(&job_desc->environment, "DUMMY_VAR", "DUMMY_VALUE") != 1)
	{
		slurm_error("[hagen-daas] Could not set dummy environment variable for scoop job.");
	    return -1;
	}
	++job_desc->env_size;

	const size_t env_name_len = strlen(hd_spank_prefix) + strlen(hd_opt_name_launch_scoop)+1;
	char env_name[env_name_len];
	memset(env_name, 0, env_name_len);
	strcat(env_name, hd_spank_prefix);
	strcat(env_name, hd_opt_name_launch_scoop);
	if (env_array_append(&job_desc->spank_job_env, env_name, board_id) != 1)
	{
		return -1;
	}
	++job_desc->spank_job_env_size;

	return 0;
}

static int _check_scoop_job_running(uint32_t job_id)
{
	int rc;

	slurm_debug("[hagen-daas] Sending SIGCONT to job #%d, ignoring any errors", job_id);
	// if the job is not running then sending the signal will obviously fail
	// but we need to send the signal prior to checking the status of the job to avoid
	// race conditions
	rc = slurm_kill_job(job_id, SIGCONT, KILL_FULL_JOB);

	slurm_debug("[hagen-daas] slurm_kill_job returned %d", rc);

	return _check_job_running(job_id);
}

static int _check_job_running(uint32_t job_id)
{
	job_info_msg_t* job_info_msg = NULL;
	int rc;

	slurm_debug("[hagen-daas] Loading scoop job info for job %d..", job_id);
	if (slurm_load_job(&job_info_msg, job_id, SHOW_ALL) != SLURM_SUCCESS)
	{
		slurm_error("[hagen-daas] Invalid job id for scoop job!");
		return -1;
	}

	if (job_info_msg->record_count != 1)
	{
		slurm_error("[hagen-daas] Not exactly one response for job_id %d", job_id);
		slurm_free_job_info_msg(job_info_msg);
		return -1;
	}

	slurm_job_info_t* job_info = job_info_msg->job_array;

	// if the job was just started quiggeldy might not be responsive yet
	if (IS_JOB_RUNNING(job_info))
	{
		slurm_debug("[hagen-daas] Job #%d is running..", job_id);
		rc = 0;
	}
	else if (IS_JOB_PENDING(job_info))
	{
		slurm_debug("[hagen-daas] Job #%d is pending..", job_id);
		rc = 1;
	}
	else
	{
		slurm_debug("[hagen-daas] Job #%d is neither running nor pending, but in state %d", job_id, job_info->job_state);
		rc = -1;
	}
	slurm_free_job_info_msg(job_info_msg);
	return rc;
}

static int _wait_for_job_id(spank_t sp, uint32_t job_id)
{
	char const dep_token[] = "after:";
	size_t const dependency_len = MAX_LENGTH_ARGUMENT + strlen(dep_token);
	char dependency[dependency_len];
	memset(dependency, 0, dependency_len);

	int rc;
	// get own job id
	uint32_t my_job_id;
	rc = spank_get_item(sp, S_JOB_ID, &my_job_id);
	if (rc != ESPANK_SUCCESS)
	{
		slurm_error("[hagen-daas] Could not determine job id of compute job.");
		return -1;
	}

	// add dependency for job to wait on
	job_desc_msg_t job_desc;
	slurm_init_job_desc_msg (&job_desc);

	job_desc.job_id = my_job_id;

	strcat(dependency, dep_token);
	snprintf(dependency + strlen(dep_token), MAX_LENGTH_ARGUMENT, "%d", job_id);

	// we overwrite all dependencies since the job was already started hence their were satisifed
	job_desc.dependency = dependency;

	rc = slurm_update_job(&job_desc);
	if (rc == SLURM_SUCCESS)
	{
		slurm_error("[hagen-daas] Could not update job dependency.");
		return -1;
	}

	// requeue in PENDING state
	rc = -1;
	while (rc != SLURM_SUCCESS)
	{
		rc = slurm_requeue(my_job_id, 0);
		if (rc == ESLURM_BATCH_ONLY)
		{
			// we cannot requeue ourselves, so we just have to wait
			size_t i;
			for (i=0; i < hd_srun_requeue_wait_num_periods; ++i)
			{
				slurm_info("[hagen-daas] Waiting for hardware control daemon to start.. Elapsed: %zus / Max: %ds",
						 i * hd_srun_requeue_wait_period_secs,
						hd_srun_requeue_wait_num_periods * hd_srun_requeue_wait_period_secs);
				sleep(hd_srun_requeue_wait_period_secs);
				if (_check_scoop_job_running(job_id) == 0)
				{
					return 0;
				}
			}
			slurm_error("[hagen-daas] Scoop did not start up. Compute job will fail..");
			// note: if we return -1 here, the node will be drained which should never happen because of a scheduling
			// conflict.
			spank_setenv(sp, hd_env_name_error_msg, "Scoop did not start!", 1);
			return 0;
		}
		else if (rc != SLURM_SUCCESS)
		{
			slurm_error("[hagen-daas] Could not requeue compute job, received RC=%d", rc);
			return -1;
		}
	}
	// Waiting for a requeue here will not work because the slurm controller will only requeue the job once the
	// spank_init-phase is done.
	return 0;
}

static int _board_id_to_scoop_job_id(char const* board_id, uint32_t* job_id)
{
	struct passwd* pwd = getpwnam(hd_scoop_job_user); // no need to free
	if (pwd == NULL)
	{
		slurm_error("[hagen-daas] Failed to get uid/gid for hagen-daas user.");
		return -1;
	}
	uint32_t scoop_job_uid = pwd->pw_uid;

	int rc;
	size_t i;
	static job_info_msg_t *job_ptr = NULL;

	size_t const jobname_len = strlen(hd_scoop_jobname_prefix) + MAX_LENGTH_ARGUMENT;
	char jobname[jobname_len];
	memset(jobname, 0, jobname_len);
	strcat(jobname, hd_scoop_jobname_prefix);
	strcat(jobname, board_id);

	// Note: The plugin gets reloaded for every job, so we unfortunately cannot store and resuse the job information we
	// retrieve here.
	rc = slurm_load_job_user(&job_ptr, scoop_job_uid, SHOW_ALL);
	if (rc != SLURM_SUCCESS)
	{
		slurm_error("[hagen-daas] Failed to retrieve jobs for user %s (uid %d), RC: %d", hd_scoop_job_user, scoop_job_uid, rc);
		return -1;
	}
	job_info_t* job = job_ptr->job_array - 1; 
	for (i = 0; i < job_ptr->record_count; ++i)
	{
		++job;
		slurm_debug("[hagen-daas] Looking at job #%d with name %s in state %d", job->job_id, job->name, job->job_state);
		if (!(IS_JOB_RUNNING(job) || IS_JOB_PENDING(job)))
		{
			continue;
		}

		if (strcmp(job->name, jobname) == 0)
		{
			break;
		}
	}
	if (i >= job_ptr->record_count)
	{
		job = NULL;
	}
	else
	{
		*job_id = job->job_id;
	}

	slurm_free_job_info_msg(job_ptr);

	if (job == NULL)
	{
		slurm_error("[hagen-daas] Did not find scoop job for board %s!", board_id);
		return -1;
	}
	else
	{
		return 0;
	}
}

static int _find_wait_existing_scoop_job(spank_t sp)
{
	int rc;
	uint32_t job_id;
	char board_id[MAX_LENGTH_ARGUMENT];

	if (spank_getenv(sp, hd_env_name_scoop_board_id, board_id, MAX_LENGTH_ARGUMENT) != ESPANK_SUCCESS)
	{
		slurm_error("[hagen-daas] Failed to get %s!", hd_env_name_scoop_board_id);
		return -1;
	}

	if (_board_id_to_scoop_job_id(board_id, &job_id) != 0)
	{
		// no job id found
		return -1;
	}

	rc = _check_scoop_job_running(job_id);
	if (rc == 0)
	{
		// scoop is running -> nothing to do
		return 0;
	}
	else if (rc == 1)
	{
		// scoop was launched but is pending -> wait for it
		return _wait_for_job_id(sp, job_id);
	}
	else
	{
		// scoop already terminated
		return -1;
	}
}

// vim: sw=4 ts=4 sts=4 noexpandtab tw=120
