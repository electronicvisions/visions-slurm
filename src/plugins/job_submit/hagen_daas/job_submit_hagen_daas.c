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
#include <sys/stat.h>
#include <sys/types.h>
#include <linux/limits.h>

#include "slurm/slurm_errno.h"
#include "src/common/list.h"
#include "src/common/env.h"
// #include "src/common/slurm_xlator.h"
#include "src/common/log.h"
#include "src/common/node_conf.h"
#include "src/common/xstring.h"
#include "src/slurmctld/slurmctld.h"

#define SPANK_OPT_PREFIX "_SLURM_SPANK_OPTION_hagen_daas_opts_"

#define MAX_NUM_ARGUMENTS 64
#define MAX_LENGTH_ARGUMENT_CHAIN 16384 // max number of chars for one argument
										// chain
#define MAX_LENGTH_ARGUMENT 64          // max number of chars for one element
										// of an argument chain
#define MAX_LENGTH_ERROR 8192
#define MAX_LENGTH_ENV_NAME 64
#define MAX_LENGTH_OPTION_NAME 64
#define MAX_LENGTH_SERVICE_NAME 64

#define HAGEN_DAAS_PLUGIN_SUCCESS 0
#define HAGEN_DAAS_PLUGIN_FAILURE -1
#define HAGEN_DAAS_PLUGIN_NO_JOB_NEEDED 1

/*
 * CONFIGURATION
 */

// quiggeldy defines
static const char env_name_quiggeldy_ip[] = "QUIGGELDY_IP";
static const char env_name_quiggeldy_port[] = "QUIGGELDY_PORT";

// hagen daas defines
const char env_name_board_id[] = "HAGEN_DAAS_BOARD";
static const char jobname_prefix_scoop[] = "quiggeldy_";
// name of the gres in the  gres plugin
static const char hagen_daas_gres_name[] = "hd_scoop";

// account to which the scoop jobs should be attributed
static const char scoops_account[] = "daemons";
static const char scoops_partition[] = "daemons";

// SLURM plugin definitions
const char plugin_name[] =
	"Job submit 'howto avoid grabbing emulators nightlong - DLS as a Service' "
	"plugin. Spawns an arbiter for each chip in use that handles experiments "
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
	char script_path[PATH_MAX];
	char slurm_account[64];
	char slurm_partition[64];
	uint16_t port;
	size_t reallocation_period;
	size_t timeout_idle;
} service_t;

#define NUM_SERVICES 1
static const service_t service_infos[NUM_SERVICES] = {
	{ "dls-v2", "/etc/slurm/scoop_sleep.sh", "daemons", "daemons", 5666, 600, 900 },
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


/* This mutex is unneeded because of g_context_lock in
 * src/slurmctld/job_submit.{h,c}, but the documentation requires plugin_methods
 * to be reentrant anyway.
 */
static pthread_mutex_t mutex_running_scoops_l = PTHREAD_MUTEX_INITIALIZER;
static List running_scoops_l;


// global string to hold error message for slurm
static char function_error_msg[MAX_LENGTH_ERROR];


/***********************\
* function declarations *
\***********************/

/* takes a string and converts if poossible to int and saves in ret returns
 * HAGEN_DAAS_PLUGIN_SUCCESS on success, HAGEN_DAAS_PLUGIN_FAILURE on failure */
static int _str2int(char const* str, int* ret);

/* takes an option string and returns corresponding index, if string is no valid
 * option returns HAGEN_DAAS_PLUGIN_FAILURE */
static int _option_lookup(char const* option_string);

/* parses the options from the spank job environment given by job_desc and
 * converts them to
 job_entries. zero_res_args is true if no spank options regarding nmpm resource
 management where found */
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
static running_scoop_t* _build_scoop(
	char const* board_id,
	struct node_record
	const* node, service_t const* service);


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


/* Adjust the job_desc_msg (e.g. Set the environment variables to point to the
 * running service so that the job can connect to it) to prepare it for
 * execution.
 *
 */
static int _modify_job_desc_compute_job(
	job_desc_msg_t* job, running_scoop_t* scoop);


/* Set script of a job description to contain the given executable of 
 *
 */
static int _set_script(job_desc_msg_t* job, char const* executable);


/* Prepare the user submitted job.
 */
static int _prepare_job(job_desc_msg_t* job, option_entry_t* parsed_options);


/* Check if scoop is running and if not modify this job to start it, otherwise
 * modify this job to do nothing.
 *
 */
static int _launch_scoop(
	job_desc_msg_t* job_desc,
	option_entry_t* parsed_options);


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


/* Launch the given scoop configuration in a job and set the returned job id in
 * scoop
 */
static int _modify_job_desc_launch_scoop(
		job_desc_msg_t* job_desc,
		running_scoop_t* scoop,
		struct node_record* node);


/* Takes a slurm address and converts it to a string representing the ip
 */
static char* _addr2ip(slurm_addr_t const* addr);


/* Check if the supplied job_desc_msg_t is a batch script.
 *
 * This is just a precaution as scoops should only be started via the gres-type
 * plugin (i.e. no user interaction).
 */
static bool _job_desc_is_batch_job(job_desc_msg_t* job_desc);


/* Append gres value to a possibly existing gres specificaiton.
 */
static int _append_gres(char** gres_location, const char* to_append);


/* Check if the job corresponding to the given scoop is still running.
 *
 * If the scoop has no job_id but the running job is found via job_name
 * then the job_id attribute will be set.
 */
static bool _check_scoop_running(running_scoop_t* scoop);


/* Remove the given scoop from the list of running scoops.
 *
 * Does not xfree the scoop!
 */
static void _remove_scoop_from_list(running_scoop_t* scoop);


/* Read contents from file into newly created buffer.
 *
 * Caller must xfree buffer.
 */
static char* _read_file(char* const fname);

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
extern int job_submit(
	struct job_descriptor* job_desc,
	uint32_t submit_uid,
	char** err_msg)
{
	size_t optioncounter;

	// holds all parsed options
	option_entry_t parsed_options[NUM_UNIQUE_OPTIONS];

	char my_errmsg[MAX_LENGTH_ERROR]; // string for temporary error message
	bool zero_res_args = true;
	int retval = SLURM_ERROR;

	// init variables
	for (optioncounter = 0; optioncounter < NUM_OPTIONS; optioncounter++)
	{
		parsed_options[optioncounter].num_arguments = 0;
	}
	strcpy(function_error_msg, "");

	// get parsed options
	if (_parse_options(job_desc, parsed_options, &zero_res_args)
		!= HAGEN_DAAS_PLUGIN_SUCCESS)
	{
		snprintf(my_errmsg, MAX_LENGTH_ERROR, "_parse_options: %s",
			function_error_msg);
		retval = SLURM_ERROR;
		goto CLEANUP;
	}

	// check if any res arg was given, if not exit successfully
	if (zero_res_args)
	{
		info("no hagen_daas resources requested.");
		retval = SLURM_SUCCESS;
		goto CLEANUP;
	}
	// start_scoop can only be specified alone 
	if ((parsed_options[_option_lookup("dbid")].num_arguments > 0) &&
			(parsed_options[_option_lookup("dbid")].num_arguments > 0))
	{
		snprintf(my_errmsg, MAX_LENGTH_ERROR, "job_submit: %s",
			"Please specify either --daas-board-id or --start-scoop.");
		retval = SLURM_ERROR;
		goto CLEANUP;
	}


	if (parsed_options[_option_lookup("dbid")].num_arguments > 0)
	{
		if (_prepare_job(job_desc, parsed_options) != HAGEN_DAAS_PLUGIN_SUCCESS)
		{
			snprintf(my_errmsg, MAX_LENGTH_ERROR, "_prepare_job: %s",
				function_error_msg);
			retval = SLURM_ERROR;
			goto CLEANUP;
		}
	}

	if (parsed_options[_option_lookup("start_scoop")].num_arguments > 0)
	{
		int rc = _launch_scoop(job_desc, parsed_options);
        if (rc == HAGEN_DAAS_PLUGIN_NO_JOB_NEEDED)
		{
			// stop job allocation since the scoop is already running
			sprintf(my_errmsg, "Scoop is running, no job needed.");
			retval = SLURM_ERROR;
			goto CLEANUP;
		}
		else if (rc == HAGEN_DAAS_PLUGIN_FAILURE)
		{
			snprintf(my_errmsg, MAX_LENGTH_ERROR, "_launch_scoop: %s",
				function_error_msg);
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
	struct job_descriptor* job_desc,
	struct job_record* job_ptr,
	uint32_t submit_uid)
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
	if (end == str
		|| *end != '\0'
		|| (errno == ERANGE && (value == LONG_MAX || value == LONG_MIN)))
	{
		return HAGEN_DAAS_PLUGIN_FAILURE;
	}
	if (value > INT_MAX || value < INT_MIN)
	{
		return HAGEN_DAAS_PLUGIN_FAILURE;
	}
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
		if (strcmp(custom_plugin_options[indexcounter].option_name,
				option_string) == 0)
		{
			return custom_plugin_options[indexcounter].index;
		}
	}
	return HAGEN_DAAS_PLUGIN_FAILURE;
}

// adapted from src/plugins/job_submit/nmpm_custom_resource/job_submit_nmpm_custom_resource.c
static int _parse_options(
	struct job_descriptor const* job_desc,
	option_entry_t* parsed_options,
	bool* zero_res_args)
{
	size_t optioncount, argcount;
	char argumentsrc[MAX_LENGTH_ARGUMENT_CHAIN];
	char* arguments;
	char* option;
	char* argument_token;
	// each option is formated the following way
	// _SLURM_SPANK_OPTION_hagen_daas_opts_[option]=[argument,argument,...]
	// we iterate over all arguments of all options and save them in
	// parsed_options
	for (optioncount=0; optioncount<job_desc->spank_job_env_size; optioncount++)
	{
		char* spank_option_str = job_desc->spank_job_env[optioncount];
		info("Trying option: %s", spank_option_str);
		option = strstr(spank_option_str, SPANK_OPT_PREFIX);
		if (option == NULL)
		{
			// some other spank option, skip
			continue;
		}
		option += strlen(SPANK_OPT_PREFIX); // truncate SPANK_OPT_PREFIX
		strncpy(argumentsrc, option, MAX_LENGTH_ARGUMENT_CHAIN);
		arguments = strstr(argumentsrc, "="); // get string after = symbol
		if (arguments == NULL)
		{
			snprintf(
				function_error_msg, MAX_LENGTH_ERROR,
				"'=' not present in spank option string, "
				"this should never happen");
			return HAGEN_DAAS_PLUGIN_FAILURE;
		}
		// truncate '=' at end of option string
		option[strlen(option) - strlen(arguments)] = 0;
		// truncate '=' at beginning of argument chain
		arguments += 1;
		if (strlen(arguments) > MAX_LENGTH_ARGUMENT_CHAIN)
		{
			snprintf(
				function_error_msg, MAX_LENGTH_ERROR,
				"Too long argument, over %d chars",
				MAX_LENGTH_ARGUMENT_CHAIN);
			return HAGEN_DAAS_PLUGIN_FAILURE;
		}

		argument_token = strtok(arguments, ",");
		argcount = 0;
		if (_option_lookup(option) < 0)
		{
			snprintf(
				function_error_msg, MAX_LENGTH_ERROR,
				"Invalid option %s, please update spank arguments", option);
			return HAGEN_DAAS_PLUGIN_FAILURE;
		}
		*zero_res_args = false;
		while (argument_token != NULL)
		{
			strcpy(parsed_options[_option_lookup(option)].arguments[argcount],
				argument_token);
			argcount++;
			parsed_options[_option_lookup(option)].num_arguments = argcount;
			argument_token = strtok(NULL, ",");
		}
	}
	return HAGEN_DAAS_PLUGIN_SUCCESS;
}

static int _modify_job_desc_compute_job(
	job_desc_msg_t* job_desc, running_scoop_t* scoop)
{
	info("# elements in env (var): %d", job_desc->env_size);
	if (job_desc->environment != NULL)
	{
		info("# elements in env (xsize): %zu",
			xsize(job_desc->environment) / sizeof(char*));
		info("last element in env: %s",
			job_desc->environment[job_desc->env_size-1]);
		info("last+1 element in env: %s",
			job_desc->environment[job_desc->env_size]);
	}
	info("job_desc->environment (pre): %p", (void*) job_desc->environment);

	if (env_array_append(&job_desc->environment, env_name_quiggeldy_ip,
		scoop->ip) != 1)
	{
		return HAGEN_DAAS_PLUGIN_FAILURE;
	}
	++job_desc->env_size;

	if (env_array_append_fmt(&job_desc->environment, env_name_quiggeldy_port,
		"%d", scoop->service->port) != 1)
	{
		return HAGEN_DAAS_PLUGIN_FAILURE;
	}
	++job_desc->env_size;

	if (env_array_append(&job_desc->environment, env_name_board_id,
			scoop->board_id) != 1)
	{
		return HAGEN_DAAS_PLUGIN_FAILURE;
	}
	++job_desc->env_size;

	info("job_desc->environment (post): %p", (void*) job_desc->environment);
	info("# elements in env (xsize, post): %zu",
		xsize(*job_desc->environment) / sizeof(char*));

	size_t i;
	for (i=0; i<job_desc->env_size; ++i)
	{
		info("#%zu: %s", i, job_desc->environment[i]);
	}

	info("last element in env (post): %s",
			job_desc->environment[job_desc->env_size-1]);
	info("last+1 element in env (post): %s",
			job_desc->environment[job_desc->env_size]);

	// we need to be able to requeue the job if scoop allocation fails
	job_desc->requeue = 1;

	if (_append_gres(&job_desc->gres, (char * const) hagen_daas_gres_name)
		!= HAGEN_DAAS_PLUGIN_SUCCESS)
	{
		return HAGEN_DAAS_PLUGIN_FAILURE;
	}

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
		if (strncmp(service_name, service_infos[i].service_name,
				MAX_LENGTH_SERVICE_NAME) == 0)
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

	// helper variables for string comparison because strtok_r replaces
	// seperators by NULL
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
		// make copy because strtok_r replaces seperator in the original string
		// with 0
		gres_cpy_outter = xstrdup(node_record_table_ptr[i].config_ptr->gres);
		info("Gres string for node %s: %s",
			node_record_table_ptr[i].node_hostname, gres_cpy_outter);
		for(gres_node = strtok_r(gres_cpy_outter, gres_sep, &buf_outter);
			gres_node;
			gres_node = strtok_r(NULL, gres_sep, &buf_outter))
		{
			// if the gres configuration contains counts or the
			// ':no_consume'-tag, comparison will fail -> gres name is the name
			// up until the first colon
			gres_cpy_inner = xstrdup(gres_node);
			gres_found = xstrcmp(strtok_r(gres_cpy_inner, ":", &buf_inner),
					gres) == 0;
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

static int _prepare_job(
		struct job_descriptor* job_desc, option_entry_t* parsed_options)
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

	// first, see if the scoop is already running and get the information from
	// there note that we do not lock the mutex_running_scoops_l because there
	// cant be a race condition -> the node where the scoop is located does not
	// depend on whether it is running
	running_scoop_t* scoop = _board_id_to_scoop(board_id);
	bool scoop_running = scoop != NULL;

	if (!scoop_running)
	{
		// if the scoop is not running, we have to look up where it would run so
		// that the job has these information
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

	int retval = _modify_job_desc_compute_job(job_desc, scoop);
	if (!scoop_running)
	{
		// if the scoop is not running we build a temporary placeholder -> free
		// it
		xfree(scoop);
	}
	return retval;
}

static int _launch_scoop(
		job_desc_msg_t* job_desc, option_entry_t* parsed_options)
{
	if (!_job_desc_is_batch_job(job_desc))
	{
		snprintf(
			function_error_msg, MAX_LENGTH_ERROR,
			"start-scoop command not supplied via sbatch.");
		return HAGEN_DAAS_PLUGIN_FAILURE;
	}

	option_entry_t* option_scoop =
		&parsed_options[_option_lookup("start_scoop")];

	if (option_scoop->num_arguments > 1)
	{
		snprintf(
			function_error_msg, MAX_LENGTH_ERROR,
			"Only one scoop can be started at the same time.");
		return HAGEN_DAAS_PLUGIN_FAILURE;
	}

	char const* board_id = option_scoop->arguments[0];

	running_scoop_t* scoop;

	slurm_mutex_lock(&mutex_running_scoops_l);
	// check if scoop is running
	scoop = _board_id_to_scoop(board_id);

	if (scoop != NULL)
	{
		// TODO verify that job is still running
		if (!_check_scoop_running(scoop))
		{
			_remove_scoop_from_list(scoop);
			xfree(scoop);
			scoop = NULL;
		}
		// TODO think about possible race condition if the job terminates
		// possible solution: have another utility send a ping..
	}

	// if scoop is running -> nothing to do
	if (scoop == NULL)
	{
		// if the scoop is not running, we have to look up where it would run so
		// that the job has these information
		service_t const* service = _board_id_to_service(board_id);
		struct node_record* node = _gres_to_node(board_id);

		scoop = _build_scoop(board_id, node, service);

		// _modify_job_desc_launch_scoop sets the job_id
		if ((_modify_job_desc_launch_scoop(job_desc, scoop, node)
			!= HAGEN_DAAS_PLUGIN_SUCCESS))
		{
			slurm_mutex_unlock(&mutex_running_scoops_l);
			return HAGEN_DAAS_PLUGIN_FAILURE;
		}
		list_append(running_scoops_l, scoop);
	}
	else
	{
		info("Scoop is already running in job #%d", scoop->job_id);
		slurm_mutex_unlock(&mutex_running_scoops_l);
		return HAGEN_DAAS_PLUGIN_NO_JOB_NEEDED;
	}

	slurm_mutex_unlock(&mutex_running_scoops_l);
	return HAGEN_DAAS_PLUGIN_SUCCESS;
}

static int _modify_job_desc_launch_scoop(
		job_desc_msg_t* job_desc,
		running_scoop_t* scoop,
		struct node_record* node)
{
	if (scoop == NULL)
	{
		return HAGEN_DAAS_PLUGIN_FAILURE;
	}

	if (job_desc->script != NULL)
	{
		// if there was a script present, replace it
		xfree(job_desc->script);
	}
	job_desc->script = _read_file(scoop->service->script_path);

	job_desc->req_nodes = xstrdup(node->node_hostname);

	if (job_desc->name != NULL)
	{
		xfree(job_desc->name);
	}
	size_t name_len = strlen(jobname_prefix_scoop)
					+ strlen(scoop->board_id) + 1;
	job_desc->name = xmalloc(sizeof(char) * name_len);
	memset(job_desc->name, 0, name_len);
	sprintf(job_desc->name, "%s%s", jobname_prefix_scoop, scoop->board_id);

	job_desc->user_id = getuid();
	job_desc->group_id = getgid();
	job_desc->account = xstrdup(scoop->service->slurm_account);
	job_desc->partition = xstrdup(scoop->service->slurm_partition);

	// TODO set paths for stdout/stderr to some configurable systempath!

	// TODO set up clean new environment -> scoop script has to do the heavy

	/* env_array_for_batch_job */
	// lifting
	/*
	 * extern char** environ;
	 * job_desc->environment = env_array_create();
	 * env_array_merge_slurm(&job_desc->environment, (const char **)environ);
	 */

	// TODO: set further requirements such as partitions etc
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

static bool _job_desc_is_batch_job(job_desc_msg_t* job_desc)
{
	// If submitted via srun the environment will be NULL 
	// TODO find more reliable way to test this
	return (job_desc->environment != NULL);
}

static int _append_gres(char** gres_location, const char* to_append)
{
	size_t len_gres = 0;
	char* old_gres = *gres_location;
	if (old_gres != NULL)
	{
		len_gres += strlen(old_gres) + 1; // additional seperator 
	}
	len_gres += strlen(to_append);

	*gres_location = xmalloc(sizeof(char) * len_gres);
	if (*gres_location == NULL)
	{
		return HAGEN_DAAS_PLUGIN_FAILURE;
	}
	memset(*gres_location, 0, len_gres);
	
	if (old_gres != NULL)
	{
		sprintf(*gres_location, "%s,", old_gres);
	}
	xstrcat(*gres_location, to_append);

	if (old_gres != NULL)
	{
		xfree(old_gres);
	}

	return HAGEN_DAAS_PLUGIN_SUCCESS;
}

static bool _check_scoop_running(running_scoop_t* scoop)
{
	// TODO
}

static void _remove_scoop_from_list(running_scoop_t* scoop)
{
	// TODO
}


static char* _read_file(char * const fname)
{
	int fd, i, offset = 0;
	struct stat stat_buf;
	char *file_buf;

	fd = open(fname, O_RDONLY);
	if (fd < 0) {
		fatal("Could not open scoop script %s: %m", fname);
	}
	if (fstat(fd, &stat_buf) < 0) {
		fatal("Could not stat scoop script %s: %m", fname);
	}
	file_buf = xmalloc(stat_buf.st_size);
	while (stat_buf.st_size > offset) {
		i = read(fd, file_buf + offset, stat_buf.st_size - offset);
		if (i < 0) {
			if (errno == EAGAIN)
				continue;
			fatal("Could not read scoop script file %s: %m", fname);
		}
		if (i == 0)
			break;	/* EOF */
		offset += i;
	}
	close(fd);
	return file_buf;
}

// vim: sw=4 ts=4 sts=4 noexpandtab tw=80
