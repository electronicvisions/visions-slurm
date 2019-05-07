#pragma once

// Common defines used in several plugins

#include <limits.h>
#include <slurm/slurm.h>

/*********************************************
 * common compile-time defines and constants *
 *********************************************/

#define HAGEN_DAAS_OPT_NAME_LAUNCH_SCOOP "launch_scoop"
#define HAGEN_DAAS_SPANK_PREFIX "_SLURM_SPANK_OPTION_hagen_daas_"

#define HAGEN_DAAS_PLUGIN_SUCCESS 0
#define HAGEN_DAAS_PLUGIN_FAILURE -1
#define HAGEN_DAAS_PLUGIN_NO_JOB_NEEDED 1

#define MAX_NUM_ARGUMENTS 64
#define MAX_LENGTH_ARGUMENT_CHAIN 16384 // max number of chars for one argument
#define MAX_LENGTH_ARGUMENT 64          // max number of chars for one element of an argument chain
#define MAX_LENGTH_ERROR 8192
#define MAX_LENGTH_OPTION_NAME 64

/**************************
 * common data structures *
 *************************/

typedef struct service
{
	char* name;
	char* script_path;
	char* slurm_account;
	char* slurm_partition;
	uint16_t port;
	uint16_t num_cpus; // how many cpus does the scoop job need
	uint64_t memory_in_mb;
	char** board_ids;
	size_t num_board_ids;
} service_t;

// all configuration in one file
typedef struct hd_config
{
	// environment defines
	char* env_content_magic;
	char* env_name_magic;
	char* env_name_scoop_board_id;
	char* env_name_scoop_ip;
	char* env_name_scoop_job_id;
	char* env_name_scoop_port;
	char* env_name_error_msg;

	// services
	service_t* services;
	uint32_t num_services;

	// hagen daas defines
	// jobname fmt specifier having one string placeholder for the board_id
	char* scoop_jobname_prefix;

	char* scoop_job_user;

	// working directory into which slurm logs etc are being placed
	char* scoop_working_dir;

	// how many seconds does a compute job wait once the scoop has been started?
	int scoop_launch_wait_secs;

	// how many seconds is a started scoop job still considered pending
	int scoop_pending_secs;

	// time to wait till checking again if scoop is running in srun calls
	int srun_requeue_wait_period_secs;
	int srun_requeue_wait_num_periods;

	// time to wait for scoop launch job to appear in queue
	int scoop_launch_wait_period_secs;
	int scoop_launch_wait_num_periods;
} hd_config_t;

// initialize hd_config_t data structure
void hd_config_t_init(hd_config_t**);

// free hd_config_t data structure
void hd_config_t_free(hd_config_t**);

// load hd_config_t from file
int hd_config_t_load(hd_config_t**);

/* Get service by board id.
 *
 * Returns nullpointer if service not found.
 */
const service_t* board_id_to_service(char const* board_id);

extern hd_config_t* hagen_daas_config;

// vim: sw=4 ts=4 sts=4 noexpandtab tw=80
