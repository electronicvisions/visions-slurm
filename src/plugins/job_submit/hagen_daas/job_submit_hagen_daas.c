/*****************************************************************************\
 *  hagen_daas.c- howto avoid grabbing emulators nightlong - DLS as a Service
 *  mitigates hardware access by ensuring the corresponding arbiter for the
 *  board is running and sets the environment variables so that ip and port
 *  for the user software is known
\*****************************************************************************/

#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>

#include "slurm/slurm_errno.h"
#include "src/common/list.h"
#include "src/common/env.h"
// #include "src/common/slurm_xlator.h"
#include "src/common/log.h"
#include "src/common/xstring.h"
#include "src/slurmctld/slurmctld.h"

#define SPANK_OPT_PREFIX "_SLURM_SPANK_OPTION_hagen_daas_opts_"

#define NUM_FPGAS_ON_WAFER 48
#define NUM_HICANNS_ON_WAFER 384
#define MAX_ADCS_PER_WAFER 12

#define MAX_NUM_ARGUMENTS 1 // currently only a single resource may be specified
#define MAX_LENGTH_ARGUMENT_CHAIN 10000 // max number of chars for one argument chain
#define MAX_LENGTH_ARGUMENT 50          // max number of chars for one element of an argument chain
#define MAX_LENGTH_ERROR 5000
#define MAX_LENGTH_ENV_NAME 50
#define MAX_LENGTH_OPTION_NAME 64
#define MAX_LENGTH_SERVICE_NAME 64
#define MAX_LENGTH_EXECUTABLE_PATH 64

#define HAGEN_DAAS_PLUGIN_SUCCESS 0
#define HAGEN_DAAS_PLUGIN_FAILURE -1

// quiggeldy defines

#define ENV_NAME_QUIGGELDY_IP "QUIGGELDY_IP"
#define ENV_NAME_QUIGGELDY_PORT "QUIGGELDY_PORT"

#define JOBNAME_PREFIX_QUIGGELDY "quiggeldy_"

// SLURM plugin definitions
const char plugin_name[] = "Job submit 'howto avoid grabbing emulators nightlong - DLS as a Service' plugin. "
						   "Spawns an arbiter for each chip in use that handles experiments "
						   "in order to increase through-put.";
const char plugin_type[] = "job_submit/hagen_daas";
const uint32_t plugin_version = SLURM_VERSION_NUMBER;
const uint32_t min_plug_version = 100;

// holds array of strings of one option entry
struct option_entry
{
	char arguments[MAX_NUM_ARGUMENTS][MAX_LENGTH_ARGUMENT];
	size_t num_arguments;
};

// pair of option string and index
struct option_index_t
{
	char option_name[MAX_LENGTH_OPTION_NAME];
	int index;
};

// global array of valid options
#define NUM_OPTIONS 2
#define NUM_UNIQUE_OPTIONS 1
// Please note: dashes get converted to underscores
static const struct option_index_t custom_plugin_options[NUM_OPTIONS] = {
	{ "dbid",          0},
	{ "daas_board_id", 0},
};

typedef struct service
{
	char service_name[MAX_LENGTH_SERVICE_NAME];
	char executable[MAX_LENGTH_EXECUTABLE_PATH];
	uint16_t port;
	size_t reallocation_period;
	size_t timeout_idle;
} service_t;

#define NUM_SERVICES 3
static const service_t service_infos[NUM_SERVICES] = {
	{ "dls-v2",   "quiggeldy", 5666, 600, 900 },
	{ "dls-v3",   "quiggeldy", 5667, 600, 900 },
	{ "hicann-x", "quiggeldy", 5668, 600, 900 },
};

typedef struct running_daemon
{
	char board_id[64];
	char ip[15];
	uint16_t port;
	uint32_t job_id;
} running_daemon_t;

static void _destroy_running_daemon(void* ptr)
{
	running_daemon_t* cast = ptr;
	xfree(cast);
}

static List running_daemons_l;

// global string to hold error message for slurm
static char function_error_msg[MAX_LENGTH_ERROR];


/***********************\
* function declarations *
\***********************/

/* takes a string and converts if poossible to int and saves in ret
 * returns HAGEN_DAAS_PLUGIN_SUCCESS on success, HAGEN_DAAS_PLUGIN_FAILURE on failure */
static int _str2int(char const* str, int* ret);

/* takes an option string and returns corresponding index, if string is no valid option returns
 * HAGEN_DAAS_PLUGIN_FAILURE */
static int _option_lookup(char const* option_string);

/* parses the options from the spank job environment given by job_desc and converts them to
 job_entries. zero_res_args is true if no spank options regarding nmpm resource management where
 found */
static int _parse_options(
	struct job_descriptor const* job_desc,
	struct option_entry* parsed_options,
	bool* zero_res_args);

/* Maps board_id to a pointer to the service to run.
 *
 * Returns null pointer if mapping not possible.
 *
 * Currently we map all board ids to dls-v2 (ignoring th provided board id).
 *
 * TODO: Integrate into hwdb-like service that maps board-id to service type 
 */
static service_t const* _board_id_to_service(char const* board_id);


/* Maps board_id to a pointer to the service name to run.
 *
 * Returns null pointer if mapping not possible.
 *
 * Currently we map all board ids to dls-v2 (ignoring th provided board id).
 *
 * TODO: Integrate into hwdb-like service that maps board-id to service type 
 */
static char const* _board_id_to_service_name(char const* board_id);


/* Get service by name
 *
 * Returns nullpointer if service not found.
 */
static const service_t* _get_service(char const* service_name);


/* Set the environment variables to point to the running service so that the job can connect to it.
 *
 */
int _set_hagen_daas_env(job_desc_msg_t* job, running_daemon_t* daemon);

/***********************\
* function definition *
\***********************/

// slurm required functions
int init(void)
{
	running_daemons_l = list_create(_destroy_running_daemon);
	info("Loaded %s", plugin_type);
	return SLURM_SUCCESS;
}
void fini(void)
{
	list_destroy(running_daemons_l);
}

// main plugin function
extern int job_submit(struct job_descriptor* job_desc, uint32_t submit_uid, char** err_msg)
{
	size_t optioncounter, argcount;
	struct option_entry parsed_options[NUM_UNIQUE_OPTIONS]; // holds all parsed options
	char my_errmsg[MAX_LENGTH_ERROR]; // string for temporary error message
	bool zero_res_args = true;
	int retval = SLURM_ERROR;

	// init variables
	for (optioncounter = 0; optioncounter < NUM_OPTIONS; optioncounter++) {
		parsed_options[optioncounter].num_arguments = 0;
	}
	strcpy(function_error_msg, "");

	// get parsed options
	if (_parse_options(job_desc, parsed_options, &zero_res_args) != HAGEN_DAAS_PLUGIN_SUCCESS) {
		snprintf(my_errmsg, MAX_LENGTH_ERROR, "_parse_options: %s", function_error_msg);
		retval = SLURM_ERROR;
		goto CLEANUP;
	}

	// check if any res arg was given, if not exit successfully
	if (zero_res_args) {
		info("no hagen_daas resources requested.");
		retval = SLURM_SUCCESS;
		goto CLEANUP;
	}

	running_daemon_t mock_daemon = {
		"ritter-kunibert",
		"127.0.0.1",
		2222,
		12345
	};

	if (_set_hagen_daas_env(job_desc, &mock_daemon) != HAGEN_DAAS_PLUGIN_SUCCESS)
	{
		snprintf(my_errmsg, MAX_LENGTH_ERROR, "_set_hagen_daas_env: %s", function_error_msg);
		retval = SLURM_ERROR;
		goto CLEANUP;
	}

	retval = SLURM_SUCCESS;

CLEANUP:

	if (retval != SLURM_SUCCESS) {
		*err_msg = xstrdup(my_errmsg);
		error("%s", my_errmsg);
	}
	return retval;
}

extern int job_modify(
	struct job_descriptor* job_desc, struct job_record* job_ptr, uint32_t submit_uid)
{
	return SLURM_SUCCESS;
}

static int _str2int(char const* str, int* p2int)
{
	long int value;
	char* end;

	if (str == NULL)
		return HAGEN_DAAS_PLUGIN_FAILURE;
	errno = 0;
	value = strtol(str, &end, 10);
	if (end == str || *end != '\0' || (errno == ERANGE && (value == LONG_MAX || value == LONG_MIN)))
		return HAGEN_DAAS_PLUGIN_FAILURE;
	if (value > INT_MAX || value < INT_MIN)
		return HAGEN_DAAS_PLUGIN_FAILURE;
	*p2int = (int) value;
	return HAGEN_DAAS_PLUGIN_SUCCESS;
}

static int _option_lookup(char const* option_string)
{
	size_t indexcounter;
	for (indexcounter = 0; indexcounter < NUM_OPTIONS; indexcounter++) {
		if (strcmp(custom_plugin_options[indexcounter].option_name, option_string) == 0) {
			return custom_plugin_options[indexcounter].index;
		}
	}
	return HAGEN_DAAS_PLUGIN_FAILURE;
}

// adapted from src/plugins/job_submit/nmpm_custom_resource/job_submit_nmpm_custom_resource.c
static int _parse_options(
	struct job_descriptor const* job_desc, struct option_entry* parsed_options, bool* zero_res_args)
{
	size_t optioncount, argcount;
	char argumentsrc[MAX_LENGTH_ARGUMENT_CHAIN];
	char* arguments;
	char* option;
	char* argument_token;
	// each option is formated the following way
	// _SLURM_SPANK_OPTION_hagen_daas_opts_[option]=[argument,argument,...]
	// we iterate over all arguments of all options and save them in parsed_options
	for (optioncount = 0; optioncount < job_desc->spank_job_env_size; optioncount++) {
		char* spank_option_str = job_desc->spank_job_env[optioncount];
		info("Trying option: %s", spank_option_str);
		option = strstr(spank_option_str, SPANK_OPT_PREFIX);
		if (option == NULL) {
			// some other spank option, skip
			continue;
		}
		option += strlen(SPANK_OPT_PREFIX); // truncate SPANK_OPT_PREFIX
		strncpy(argumentsrc, option, MAX_LENGTH_ARGUMENT_CHAIN);
		arguments = strstr(argumentsrc, "="); // get string after = symbol
		if (arguments == NULL) {
			snprintf(
				function_error_msg, MAX_LENGTH_ERROR,
				"'=' not present in spank option string, this should never happen");
			return HAGEN_DAAS_PLUGIN_FAILURE;
		}
		option[strlen(option) - strlen(arguments)] = 0; // truncate '=' at end of option string
		arguments += 1; // truncate '=' at beginning of argument chain
		if (strlen(arguments) > MAX_LENGTH_ARGUMENT_CHAIN) {
			snprintf(
				function_error_msg, MAX_LENGTH_ERROR, "Too long argument, over %d chars",
				MAX_LENGTH_ARGUMENT_CHAIN);
			return HAGEN_DAAS_PLUGIN_FAILURE;
		}

		argument_token = strtok(arguments, ",");
		argcount = 0;
		if (_option_lookup(option) < 0) {
			snprintf(
				function_error_msg, MAX_LENGTH_ERROR,
				"Invalid option %s, please update spank arguments", option);
			return HAGEN_DAAS_PLUGIN_FAILURE;
		}
		*zero_res_args = false;
		while (argument_token != NULL) {
			strcpy(parsed_options[_option_lookup(option)].arguments[argcount], argument_token);
			argcount++;
			parsed_options[_option_lookup(option)].num_arguments = argcount;
			argument_token = strtok(NULL, ",");
		}
	}
	return HAGEN_DAAS_PLUGIN_SUCCESS;
}

int _set_hagen_daas_env(job_desc_msg_t* job, running_daemon_t* daemon)
{
	/*
	 * if (job->environment == NULL)
	 * {
	 *     snprintf(function_error_msg, MAX_LENGTH_ERROR, "Could not set environment!");
	 *     return HAGEN_DAAS_PLUGIN_FAILURE;
	 * }
	 */
	info("# elements in env (var): %d", job->env_size);
	if (job->environment != NULL)
	{
		info("# elements in env (xsize): %zu", xsize(job->environment) / sizeof(char*));
		info("last element in env: %s", job->environment[job->env_size-1]);
		info("last+1 element in env: %s", job->environment[job->env_size]);
	}
	info("job->environment (pre): %p", (void*) job->environment);

	/*
	 * info("job->pelog_env (pre): %p", (void*) job->pelog_env);
	 * info("# elements in pelog_env (var): %d", job->pelog_env_size);
	 */

	if (env_array_append(&job->environment, ENV_NAME_QUIGGELDY_IP, daemon->ip) != 1)
	{
		return HAGEN_DAAS_PLUGIN_FAILURE;
	}
	if (env_array_append_fmt(&job->environment, ENV_NAME_QUIGGELDY_PORT, "%d", daemon->port) != 1)
	{
		return HAGEN_DAAS_PLUGIN_FAILURE;
	}
	info("job->environment: %p", (void*) job->environment);
	info("# elements in env (xsize, post): %zu", xsize(*job->environment) / sizeof(char*));

	job->env_size += 2;

	size_t i;
	for (i=0; i<job->env_size; ++i)
	{
		info("#%zu: %s", i, job->environment[i]);
	}

	info("last element in env (post): %s", job->environment[job->env_size-1]);
	info("last+1 element in env (post): %s", job->environment[job->env_size]);

	return HAGEN_DAAS_PLUGIN_SUCCESS;
}

static service_t const* _board_id_to_service(char const* board_id)
{
	return _get_service(_board_id_to_service_name(board_id));
}


char const* _default_service_name = "dls-v2";

static char const* _board_id_to_service_name(char const* board_id)
{
	// TODO: implement me
	return _default_service_name;
}

static service_t const* _get_service(char const* service_name)
{
	size_t i;
	for (i = 0; i < NUM_SERVICES; ++i)
	{
		if (strncmp(service_name, service_infos[i].service_name, MAX_LENGTH_SERVICE_NAME) == 0)
		{
			return &service_infos[i];
		}
	}
	return NULL;
}

// vim: sw=4 ts=4 sts=4 noexpandtab tw=120
