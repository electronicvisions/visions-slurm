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
#include "src/common/node_conf.h"
#include "src/common/xstring.h"
#include "src/slurmctld/slurmctld.h"
#include "src/slurmctld/locks.h"

#define SPANK_OPT_PREFIX "_SLURM_SPANK_OPTION_hagen_daas_opts_"

#define NUM_FPGAS_ON_WAFER 48
#define NUM_HICANNS_ON_WAFER 384
#define MAX_ADCS_PER_WAFER 12

#define MAX_NUM_ARGUMENTS 64
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

const char env_name_quiggeldy_ip[] = "QUIGGELDY_IP";
const char env_name_quiggeldy_port[] = "QUIGGELDY_PORT";


// hagen daas defines

const char env_name_board_id[] = "HAGEN_DAAS_BOARD";

const char jobname_prefix_scoop[] = "quiggeldy_";

// SLURM plugin definitions
const char plugin_name[] = "Job submit 'howto avoid grabbing emulators nightlong - DLS as a Service' plugin. "
						   "Spawns an arbiter for each chip in use that handles experiments "
						   "in order to increase through-put.";
const char plugin_type[] = "job_submit/hagen_daas";
const uint32_t plugin_version = SLURM_VERSION_NUMBER;
const uint32_t min_plug_version = 100;

// holds array of strings of one option entry
typedef struct option_entry
{
	char arguments[MAX_NUM_ARGUMENTS][MAX_LENGTH_ARGUMENT];
	size_t num_arguments;
} option_entry_t;

// pair of option string and index
typedef struct option_index
{
	char option_name[MAX_LENGTH_OPTION_NAME];
	int index;
} option_index_t;

// global array of valid options
#define NUM_OPTIONS 3
#define NUM_UNIQUE_OPTIONS 2
// Please note: dashes get converted to underscores
static option_index_t custom_plugin_options[NUM_OPTIONS] = {
	{ "dbid",          0 },
	{ "daas_board_id", 0 },
	{ "start_scoop",   1 },
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
	{ "dls-v2",   "sleep 600", 5666, 600, 900 },
	// { "dls-v2",   "quiggeldy", 5666, 600, 900 },
	{ "dls-v3",   "quiggeldy", 5667, 600, 900 },
	{ "hicann-x", "quiggeldy", 5668, 600, 900 },
};

typedef struct running_scoop
{
	char board_id[64];
	char ip[16];
	uint32_t job_id;
	service_t const* service;
} running_scoop_t;

static void _destroy_running_scoop(void* ptr)
{
	running_scoop_t* cast = ptr;
	xfree(cast);
}


// TODO add mutex/lookup slurm lock implementation
static pthread_mutex_t mutex_running_scoops_l = PTHREAD_MUTEX_INITIALIZER;
static List running_scoops_l;


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
	option_entry_t* parsed_options,
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


/* xmalloc and fill a running_scoop_t with needed information.
 *
 * Caller is responsible for free-ing.
 *
 * TODO: Take optional job-descriptor and extract job-id
 */
static running_scoop_t* _build_scoop(char const* board_id, struct node_record const* node, service_t const* service);


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
static int _set_env(job_desc_msg_t* job, running_scoop_t* scoop);


/* Set script of a job description to contain the given executable of 
 *
 */
static int _set_executable(job_desc_msg_t* job, char const* executable);


/* Prepare the user submitted job.
 *
 */
static int _prepare_job(job_desc_msg_t* job, option_entry_t* parsed_options);


/* Check if scoop is running and if not launch a job start starts it
 *
 */
static int _launch_scoops(option_entry_t* parsed_options);


/* Find which node has the given gres.
 */
static struct node_record* _gres_to_node(char const* gres);

/* Compare scoops by comparing the board_id which is unique
 */
static int _cmp_scoops_by_board_id(void *x, void* key);

/* Check if a scoop is running for the given board id, else return NULL
 *
 */
static running_scoop_t* _board_id_to_scoop(char const* board_id);


/* Launch the given scoop configuration in a job and set the returned job id in scoop
 *
 */
static int _launch_scoop_job(running_scoop_t* scoop, struct node_record* node);


/* Takes a slurm address and converts it to a string representing the ip
 */
static char* _addr2ip(slurm_addr_t const* addr);


/* Setup job_desc_msg_t to be used in a scoop job
 */
static int _scoop_job_init(job_desc_msg_t* job, running_scoop_t* scoop, struct node_record* node);

/***********************\
* function definition *
\***********************/

// slurm required functions
int init(void)
{
	running_scoops_l = list_create(_destroy_running_scoop);
	info("Loaded %s", plugin_type);
	return SLURM_SUCCESS;
}
void fini(void)
{
	slurm_mutex_lock(&mutex_running_scoops_l);
	list_destroy(running_scoops_l);
	slurm_mutex_unlock(&mutex_running_scoops_l);
	slurm_mutex_destroy(&mutex_running_scoops_l);
}

// main plugin function
extern int job_submit(struct job_descriptor* job_desc, uint32_t submit_uid, char** err_msg)
{
	size_t optioncounter;
	option_entry_t parsed_options[NUM_UNIQUE_OPTIONS]; // holds all parsed options
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

	if (parsed_options[_option_lookup("dbid")].num_arguments > 0)
	{
		if (_prepare_job(job_desc, parsed_options) != HAGEN_DAAS_PLUGIN_SUCCESS)
		{
			snprintf(my_errmsg, MAX_LENGTH_ERROR, "_prepare_job: %s", function_error_msg);
			retval = SLURM_ERROR;
			goto CLEANUP;
		}
	}

	if (parsed_options[_option_lookup("start_scoop")].num_arguments > 0)
	{
		if (_launch_scoops(parsed_options) != HAGEN_DAAS_PLUGIN_SUCCESS)
		{
			snprintf(my_errmsg, MAX_LENGTH_ERROR, "_launch_scoops: %s", function_error_msg);
			retval = SLURM_ERROR;
			goto CLEANUP;
		}
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

static char* _addr2ip(slurm_addr_t const* addr)
{
	xassert(addr->sin_family == AF_INET);

	char* retval = xmalloc(sizeof(char) * 16);
	memset(retval, 0, 16);

	unsigned char bytes[4];

    bytes[0] = (addr->sin_addr.s_addr >>  0) & 0xFF; // compiler is smart
    bytes[1] = (addr->sin_addr.s_addr >>  8) & 0xFF;
    bytes[2] = (addr->sin_addr.s_addr >> 16) & 0xFF;
    bytes[3] = (addr->sin_addr.s_addr >> 24) & 0xFF;
    snprintf(retval, 16, "%d.%d.%d.%d", bytes[0], bytes[1], bytes[2], bytes[3]);

	return retval;
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
	struct job_descriptor const* job_desc, option_entry_t* parsed_options, bool* zero_res_args)
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

static int _set_env(job_desc_msg_t* job, running_scoop_t* scoop)
{
	info("# elements in env (var): %d", job->env_size);
	if (job->environment != NULL)
	{
		info("# elements in env (xsize): %zu", xsize(job->environment) / sizeof(char*));
		info("last element in env: %s", job->environment[job->env_size-1]);
		info("last+1 element in env: %s", job->environment[job->env_size]);
	}
	info("job->environment (pre): %p", (void*) job->environment);

	if (env_array_append(&job->environment, env_name_quiggeldy_ip, scoop->ip) != 1)
	{
		return HAGEN_DAAS_PLUGIN_FAILURE;
	}
	++job->env_size;

	if (env_array_append_fmt(&job->environment, env_name_quiggeldy_port, "%d", scoop->service->port) != 1)
	{
		return HAGEN_DAAS_PLUGIN_FAILURE;
	}
	++job->env_size;

	if (env_array_append(&job->environment, env_name_board_id, scoop->board_id) != 1)
	{
		return HAGEN_DAAS_PLUGIN_FAILURE;
	}
	++job->env_size;

	info("job->environment (post): %p", (void*) job->environment);
	info("# elements in env (xsize, post): %zu", xsize(*job->environment) / sizeof(char*));

	size_t i;
	for (i=0; i<job->env_size; ++i)
	{
		info("#%zu: %s", i, job->environment[i]);
	}

	info("last element in env (post): %s", job->environment[job->env_size-1]);
	info("last+1 element in env (post): %s", job->environment[job->env_size]);

	return HAGEN_DAAS_PLUGIN_SUCCESS;
}

static int _set_executable(job_desc_msg_t* job, char const* executable)
{
	char const preamble[] = "#!/bin/sh";

	size_t length_script = strlen(preamble) + strlen(executable) + 3; // newlines below and \0

	job->script = xmalloc(sizeof(char) * length_script);

	if (job->script == NULL)
	{
		snprintf(
			function_error_msg, MAX_LENGTH_ERROR,
			"Could not set executable in scoop batch job.");
		return HAGEN_DAAS_PLUGIN_FAILURE;
	}

	// similar to sbatch --wrap
	sprintf(job->script, "%s\n\n%s\n", preamble, executable);

	return HAGEN_DAAS_PLUGIN_SUCCESS;
}

static service_t const* _board_id_to_service(char const* board_id)
{
	return _get_service(_board_id_to_service_name(board_id));
}


static char const* _default_service_name = "dls-v2";

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

static struct node_record* _gres_to_node(char const* gres)
{
	char* gres_sep = ",";
	struct node_record* retval = NULL;
	bool gres_found = false;

	// helper variables for string comparison because strtok_r replaces seperators by NULL
	char* gres_node;
	char* gres_cpy_outter;
	char* buf_outter;
	char* gres_cpy_inner;
	char* buf_inner;

	// gres attribute is not hashed -> revert to linear search
	// index over nodes
	size_t i;
	for (i=0; i < node_record_count; ++i)
	{
		if (node_record_table_ptr[i].config_ptr->gres == NULL)
		{
			continue;
		}
		// make copy because strtok_r replaces seperator in the original string with 0
		gres_cpy_outter = xstrdup(node_record_table_ptr[i].config_ptr->gres);
		info("Gres string for node %s: %s", node_record_table_ptr[i].node_hostname, gres_cpy_outter);
		for(gres_node = strtok_r(gres_cpy_outter, gres_sep, &buf_outter);
			gres_node;
			gres_node = strtok_r(NULL, gres_sep, &buf_outter))
		{
			// if the gres configuration contains counts or the ':no_consume'-tag, comparison will fail
			// -> gres name is the name up until the first colon
			gres_cpy_inner = xstrdup(gres_node);
			gres_found = xstrcmp(strtok_r(gres_cpy_inner, ":", &buf_inner), gres) == 0;
			xfree(gres_cpy_inner);

			if (gres_found)
			{
				retval = &node_record_table_ptr[i];
				break;
			}
		}
		xfree(gres_cpy_outter);
		if (gres_found)
		{
			break;
		}
	}
	return retval;
}

static int _prepare_job(struct job_descriptor* job_desc, option_entry_t* parsed_options)
{
	option_entry_t* option_dbid = &parsed_options[_option_lookup("dbid")];

	if (option_dbid->num_arguments > 1)
	{
		snprintf(
			function_error_msg, MAX_LENGTH_ERROR,
			"We currently support one experiment board per job only!");
		return HAGEN_DAAS_PLUGIN_FAILURE;
	}

	char* board_id = option_dbid->arguments[0];

	// first, see if the scoop is already running and get the information from there
	// note that we do not lock the mutex_running_scoops_l because there cant be a race condition
	// -> the node where the scoop is located does not depend on whether it is running
	running_scoop_t* scoop = _board_id_to_scoop(board_id);
	bool scoop_running = scoop != NULL;

	if (!scoop_running)
	{
		// if the scoop is not running, we have to look up where it would run so that the job has these information
		service_t const* service = _board_id_to_service(board_id);

		struct node_record* node = _gres_to_node(board_id);
		
		// DELME start
		if (node != NULL)
		{
			info("Found node %s hosting %s", node->node_hostname, board_id);
		}
		else
		{
			info("Found no node hosting %s", board_id);
		}
		// DELME stop
		
		// build the mock scoop
		scoop = _build_scoop(board_id, node, service);
	}

	int retval = _set_env(job_desc, scoop);
	if (!scoop_running)
	{
		// if the scoop is not running we build a temporary placeholder -> free it
		xfree(scoop);
	}
	return retval;
}

static int _launch_scoops(option_entry_t* parsed_options)
{
	option_entry_t* option_scoop = &parsed_options[_option_lookup("start_scoop")];
	char const* board_id;
	size_t i;
	running_scoop_t* scoop;

	slurm_mutex_lock(&mutex_running_scoops_l);
	// check if scoop is running
	for (i=0; i < option_scoop->num_arguments; ++i)
	{
		board_id = option_scoop->arguments[i];
		scoop = _board_id_to_scoop(board_id);

		if (scoop != NULL)
		{
			// TODO verify that job is still running

			// TODO think about possible race condition if the job terminates
			// possible solution: have another utility send a ping..
		}

		// if scoop is running -> nothing to do
		if (scoop == NULL)
		{
			// if the scoop is not running, we have to look up where it would run so that the job has these information
			service_t const* service = _board_id_to_service(board_id);
			struct node_record* node = _gres_to_node(board_id);

			scoop = _build_scoop(board_id, node, service);

			// _launch_scoop_job sets the job_id
			if ((_launch_scoop_job(scoop, node) != HAGEN_DAAS_PLUGIN_SUCCESS)
				|| (scoop->job_id == HAGEN_DAAS_PLUGIN_FAILURE))
			{
				slurm_mutex_unlock(&mutex_running_scoops_l);
				return HAGEN_DAAS_PLUGIN_FAILURE;
			}
			list_append(running_scoops_l, scoop);
		}
		else
		{
			info("Scoop is already running in job #%d", scoop->job_id);
		}
	}
	slurm_mutex_unlock(&mutex_running_scoops_l);

	return HAGEN_DAAS_PLUGIN_SUCCESS;
}

static int _launch_scoop_job(running_scoop_t* scoop, struct node_record* node)
{
	if (scoop == NULL)
	{
		return HAGEN_DAAS_PLUGIN_FAILURE;
	}
	info("Launching %s on %s", scoop->service->service_name, node->node_hostname);

	job_desc_msg_t job;

	if (_scoop_job_init(&job, scoop, node) != HAGEN_DAAS_PLUGIN_SUCCESS)
	{
		snprintf(
			function_error_msg, MAX_LENGTH_ERROR, "Could not init scoop job_desc_msg_t.");
		return HAGEN_DAAS_PLUGIN_FAILURE;
	}

	// In order to be able to schedule another job, we need to unlock the read lock
	// until we retake control

	/* Locks: Read config, read job, read node, read partition */
	slurmctld_lock_t job_read_lock = {
		READ_LOCK, READ_LOCK, READ_LOCK, READ_LOCK, READ_LOCK };
	unlock_slurmctld(job_read_lock);

	// check if scoop could run immediately - this should always be the case because
	// the compute node is already allocated at this point and should either be able to continue or fail
	info("_launch_scoop_job: Checking if job would run immediately..");
	if (slurm_job_will_run(&job) != SLURM_SUCCESS) {
		snprintf(
			function_error_msg, MAX_LENGTH_ERROR, "Scoop would not run immediately.");
		lock_slurmctld(job_read_lock);
		return HAGEN_DAAS_PLUGIN_FAILURE;
	}

	info("_launch_scoop_job: Submitting job..");
	submit_response_msg_t* resp = NULL;
	if (slurm_submit_batch_job(&job, &resp) != SLURM_SUCCESS)
	{
		lock_slurmctld(job_read_lock);
		return HAGEN_DAAS_PLUGIN_FAILURE;
	}
	lock_slurmctld(job_read_lock);

	scoop->job_id = resp->job_id;
	slurm_free_submit_response_response_msg(resp);
	
	return HAGEN_DAAS_PLUGIN_SUCCESS;
}

static running_scoop_t* _build_scoop(char const* board_id, struct node_record const* node, service_t const* service)
{
	running_scoop_t* scoop = xmalloc(sizeof(running_scoop_t));

	strcpy(scoop->board_id, board_id);

	char* ip = _addr2ip(&node->slurm_addr);
	strcpy(scoop->ip, ip);
	xfree(ip);

	scoop->job_id = HAGEN_DAAS_PLUGIN_FAILURE;
	scoop->service = service;

	return scoop;
}

static int _cmp_scoops_by_board_id(void* x, void* key)
{
	running_scoop_t* scoop = x;
	char const* board_id = key;

	// because everything is defined differently..
	if (xstrcmp(scoop->board_id, board_id) == 0)
	{
		return 1;
	}
	else
	{
		return 0;
	}
}

static running_scoop_t* _board_id_to_scoop(char const* board_id)
{
	return (running_scoop_t*) list_find_first(running_scoops_l, _cmp_scoops_by_board_id, (void *) board_id);
}

static int _scoop_job_init(job_desc_msg_t* job, running_scoop_t* scoop, struct node_record* node)
{
	slurm_init_job_desc_msg(job);

	if (_set_executable(job, scoop->service->executable) != HAGEN_DAAS_PLUGIN_SUCCESS)
	{
		return HAGEN_DAAS_PLUGIN_FAILURE;
	}
	job->req_nodes = xstrdup(node->node_hostname);

	size_t name_len = strlen(jobname_prefix_scoop) + strlen(scoop->board_id) + 1;
	job->name = xmalloc(sizeof(char) * name_len);
	memset(job->name, 0, name_len);
	sprintf(job->name, "%s%s", jobname_prefix_scoop, scoop->board_id);

	/* env_array_for_batch_job */
	job->user_id = getuid();

	extern char** environ;
	job->environment = env_array_create();
	env_array_merge_slurm(&job->environment,
				  (const char **)environ);

	info("Environment set to: %p", job->environment);

	// TODO: set further requirements such as partitions etc


	return HAGEN_DAAS_PLUGIN_SUCCESS;
}
// vim: sw=4 ts=4 sts=4 noexpandtab tw=120
