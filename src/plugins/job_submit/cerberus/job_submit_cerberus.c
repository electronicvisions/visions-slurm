/*****************************************************************************\
 *  job_submit_cerberus.c - have a watchdog that brutally murders newly
 *  submitted jobs in case user exceed admin-defined limits on partitions.
\*****************************************************************************/

#include <inttypes.h>
#include <limits.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <linux/limits.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "slurm/slurm_errno.h"
#include "src/common/env.h"
#include "src/common/list.h"
#include "src/common/log.h"
#include "src/common/node_conf.h"
#include "src/common/slurm_protocol_defs.h"
#include "src/common/xstring.h"
#include "src/slurmctld/slurmctld.h"

#include "cerberus_config.h"

// SLURM plugin definitions
const char plugin_name[] = "Cerberus - protect precious partitions from pesky mortals submitting "
                           "too many jobs";
const char plugin_type[] = "job_submit/cerberus";
const uint32_t plugin_version = SLURM_VERSION_NUMBER;
const uint32_t min_plug_version = 100;

static crb_config_t* cerberus_config = NULL;

/***********************\
* function declarations *
\***********************/

/* Check if the parition of job_desc is currently busy, i.e., if the user
 * already has more jobs scheduled than the maximum allowed number of jobs.
 */
static bool _check_partition_busy(
    struct job_descriptor* job_desc, uint32_t submit_uid, char** err_msg);

static watched_partition_t const* _get_watched_partition(char const* partition);

/***********************\
* function definition   *
\***********************/

// slurm required functions
int init(void)
{
	if (cerberus_config == NULL) {
		crb_config_t_load(&cerberus_config);
	}
	info("[cerberus] Loaded %s", plugin_type);
	return SLURM_SUCCESS;
}

void fini(void)
{
	if (cerberus_config != NULL) {
		crb_config_t_free(&cerberus_config);
	}
}

// main plugin function
extern int job_submit(struct job_descriptor* job_desc, uint32_t submit_uid, char** err_msg)
{
	if (_check_partition_busy(job_desc, submit_uid, err_msg)) {
		return ESLURM_PARTITION_NOT_AVAIL;
	}

	return SLURM_SUCCESS;
}

extern int job_modify(
    struct job_descriptor* job_desc, struct job_record* job_ptr, uint32_t submit_uid)
{
	return SLURM_SUCCESS;
}

static bool _check_partition_busy(
    struct job_descriptor* job_desc, uint32_t submit_uid, char** err_msg)
{
	uint32_t count_jobs = 0;
	struct job_record* job;
	bool retval = false;

	watched_partition_t const* partition = _get_watched_partition(job_desc->partition);

	if (partition == NULL) {
		// if the partition is not watched, do nothing
		return false;
	}

	ListIterator itr = list_iterator_create(job_list);
	while ((job = list_next(itr))) {
		if (xstrcmp(job->partition, partition->name) != 0) {
			continue;
		}

		if (job->user_id != submit_uid) {
			continue;
		}

		if (IS_JOB_PENDING(job) || IS_JOB_RUNNING(job)) {
			// found a job
			++count_jobs;
			if (count_jobs >= partition->num_allowed_jobs_per_user) {
				*err_msg = xstrdup_printf(
				    "The partition you have called (i.e., '%s') is temporarily "
				    "unavailable, please leave a message after the beep (and have "
				    "less than %d jobs running/scheduled on it) and we will get "
				    "back to you as soon as possible.",
				    partition->name, partition->num_allowed_jobs_per_user);

				retval = true;
				goto cleanup_check_parition;
			}
		}
	}
cleanup_check_parition:
	list_iterator_destroy(itr);
	return retval;
}

static watched_partition_t const* _get_watched_partition(char const* partition)
{
	size_t idx = 0;

	for (idx = 0; idx < cerberus_config->num_partitions; ++idx) {
		if (xstrcmp(cerberus_config->partitions[idx].name, partition) == 0) {
			return cerberus_config->partitions + idx;
		}
	}
	return NULL;
}
