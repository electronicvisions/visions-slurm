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
#include <time.h>
#include <pwd.h>
#include <linux/limits.h>

#include "slurm/slurm_errno.h"
#include "src/common/list.h"
#include "src/common/env.h"
#include "src/common/log.h"
#include "src/common/node_conf.h"
#include "src/common/slurm_protocol_defs.h"
#include "src/common/xstring.h"
#include "src/slurmctld/slurmctld.h"

#include "hagen_daas_config.h"

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
	{ "daas_board_id", 0 },
	{ HAGEN_DAAS_OPT_NAME_LAUNCH_SCOOP, 1 },
};

typedef struct running_scoop
{
	char board_id[64];
	char ip[16];
	service_t const* service;
	struct job_record const* job_record;
	time_t t_start;
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
// A list with references to running scoops
static List running_scoops_l;


// global string to hold error message for slurm
static char function_error_msg[MAX_LENGTH_ERROR];


/***********************\
* function declarations *
\***********************/

/* takes an option string and returns corresponding index, if string is no valid
 * option returns HAGEN_DAAS_PLUGIN_FAILURE */
static int _option_lookup(char const* option_string);

/* parses the options from the spank job environment given by job_desc and
 * converts them to job_entries. zero_res_args is true if no spank options
 * regarding hagen daas resource management where found. Adopted from
 * nmpm-resources plugin.
 */
static int _parse_options(
	struct job_descriptor const* job_desc,
	option_entry_t* parsed_options,
	bool* zero_res_args);

/* xmalloc and fill a running_scoop_t with needed information.
 *
 * Caller is responsible for free-ing.
 *
 * Does not connect scoop to struct job_record-container.
 */
static running_scoop_t* _build_scoop(
	char const* board_id,
	struct node_record
	const* node, service_t const* service);


/* Get service by name
 *
 * Returns nullpointer if service not found.
 */
static const service_t* _get_service(char const* service_name);


/* Adjust the job_desc_msg (e.g. set the environment variables to point to the
 * running service so that the job can connect to it) to prepare it for
 * execution.
 *
 */
static int _modify_job_desc_compute_job(
	job_desc_msg_t* job, running_scoop_t* scoop);


/* Prepare the user submitted job.
 */
static int _prepare_job(job_desc_msg_t* job, option_entry_t* parsed_options);


/* Check if scoop is running and if not modify this job to start it, otherwise
 * modify this job to do nothing.
 *
 */
static int _ensure_scoop_launched(
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
static int _launch_scoop_in_job_desc(
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


/* Get the job name for a given scoop
 *
 * Caller must xfree return value
 */
static char* _get_job_name(running_scoop_t* scoop);


/* Check if the given job name corresponds to the name of a scoop running for
 * the given board_id.
 */
static bool _check_job_name_for_board_id(
		char const* job_name, char const* board_id);


/* Find the job that runs the given scoop and set the corresponding job_record
 * attribute.
 *
 * Implicitly assumes that the board_id for the scoop is unique!
 */
static void _associate_scoop_job_record(running_scoop_t* scoop);


static struct job_record* _board_id_to_job_record(char const* board_id);


/* Read contents from file into newly created buffer.
 *
 * Caller must xfree buffer.
 */
static char* _read_file(char* const fname);


/* Dump contents of scoop list
 */
static void _dump_scoop_list(void);

/* Dump relevant information from a job record.
 */
static void _dump_job_record(struct job_record const* job);


/* Check environment of job descriptor for magic environment variable
 * and if it exists set the parsed options to the corresponding values
 *
 * Returns true if parsed_options were modified.
 */
static bool _parsed_options_from_magic_env(
	job_desc_msg_t* job_desc,
	option_entry_t* parsed_options);


/* Check if job record structure is a valid pointer by checking the validity of
 * the magic cookie.
 */
static bool _job_record_valid(struct job_record const*);

/***********************\
* function definition *
\***********************/


// slurm required functions
int init(void)
{
	l_running_scoops = list_create(_destroy_running_scoop);
	if (hagen_daas_config == NULL)
	{
		hd_config_t_load(&hagen_daas_config);
	}
	info("[hagen-daas] Loaded %s", plugin_type);
	return SLURM_SUCCESS;
}

void fini(void)
{
	slurm_mutex_lock(&mutex_running_scoops_l);
	list_destroy(l_running_scoops);
	slurm_mutex_unlock(&mutex_running_scoops_l);
	slurm_mutex_destroy(&mutex_running_scoops_l);
	if (hagen_daas_config != NULL)
	{
		hd_config_t_free(&hagen_daas_config);
	}
}

// main plugin function
extern int job_submit(
	struct job_descriptor* job_desc,
	uint32_t submit_uid,
	char** err_msg)
{
    // TODO DELME
	_dump_scoop_list();

	size_t optioncounter;

	// holds all parsed options
	option_entry_t parsed_options[NUM_UNIQUE_OPTIONS];

	size_t max_length_my_error = MAX_LENGTH_ERROR + 256;
	char my_errmsg[max_length_my_error]; // string for temporary error message
	memset(my_errmsg, 0, max_length_my_error);
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
		snprintf(my_errmsg, max_length_my_error, "_parse_options: %s",
			function_error_msg);
		retval = SLURM_ERROR;
		goto CLEANUP;
	}

	// check if any res arg was given, if not exit successfully
	if (zero_res_args) // TODO
			/* && !_parsed_options_from_magic_env(job_desc, parsed_options)) */
	{
		info("[hagen-daas] no hagen_daas resources requested.");
		retval = SLURM_SUCCESS;
		goto CLEANUP;
	}
	// launch_scoop can only be specified alone
	if ((parsed_options[_option_lookup("daas_board_id")].num_arguments > 0) &&
			(parsed_options[_option_lookup("launch_scoop")].num_arguments > 0))
	{
		snprintf(my_errmsg, max_length_my_error, "job_submit: %s",
			"Please specify either --daas-board-id or --start-scoop.");
		retval = SLURM_ERROR;
		goto CLEANUP;
	}


	if (parsed_options[_option_lookup("daas_board_id")].num_arguments > 0)
	{
		info("[hagen-daas] DAAS TASK IS: Preparing user job");
		if (_prepare_job(job_desc, parsed_options) != HAGEN_DAAS_PLUGIN_SUCCESS)
		{
			snprintf(my_errmsg, max_length_my_error, "_prepare_job: %s",
				function_error_msg);
			retval = SLURM_ERROR;
			goto CLEANUP;
		}
	}

	if (parsed_options[_option_lookup("launch_scoop")].num_arguments > 0)
	{
		info("[hagen-daas] DAAS TASK IS: Launching scoop!");
		int rc = _ensure_scoop_launched(job_desc, parsed_options);
		if (rc == HAGEN_DAAS_PLUGIN_NO_JOB_NEEDED)
		{
			// stop job allocation since the scoop is already running
			sprintf(my_errmsg, "Scoop is already running, no job needed.");
			retval = SLURM_ERROR;
			errno = ESLURM_ALREADY_DONE; // seems to have no effect
										 // TODO: Investigate
			goto CLEANUP;
		}
		else if (rc == HAGEN_DAAS_PLUGIN_FAILURE)
		{
			snprintf(my_errmsg, max_length_my_error, "_ensure_scoop_launched: %s",
				function_error_msg);
			retval = SLURM_ERROR;
			goto CLEANUP;
		}
	}

	retval = SLURM_SUCCESS;

CLEANUP:

	if (retval != SLURM_SUCCESS) {
		*err_msg = xstrdup(my_errmsg);
		error("[hagen-daas] %s", my_errmsg);
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
	char* save_ptr;
	// each option is formated the following way
	// _SLURM_SPANK_OPTION_hagen_daas_opts_[option]=[argument,argument,...]
	// we iterate over all arguments of all options and save them in
	// parsed_options
	for (optioncount=0; optioncount<job_desc->spank_job_env_size; optioncount++)
	{
		char* spank_option_str = job_desc->spank_job_env[optioncount];
		info("[hagen-daas] Trying option: %s", spank_option_str);
		option = strstr(spank_option_str, HAGEN_DAAS_SPANK_PREFIX);
		if (option == NULL)
		{
			// some other spank option, skip
			continue;
		}
		strncpy(argumentsrc, option, MAX_LENGTH_ARGUMENT_CHAIN);
		option = argumentsrc; // use copy
		option += strlen(HAGEN_DAAS_SPANK_PREFIX); // truncate SPANK_OPT_PREFIX
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

		argument_token = strtok_r(arguments, ",", &save_ptr);
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
			argument_token = strtok_r(NULL, ",", &save_ptr);
		}
	}
	return HAGEN_DAAS_PLUGIN_SUCCESS;
}

static int _modify_job_desc_compute_job(
	job_desc_msg_t* job_desc, running_scoop_t* scoop)
{

	// set magic environment variable so that the spank plugin can identify jobs
	// (and especially can tell scoop jobs from compute jobs)
	if (env_array_append(&job_desc->environment,
						 hagen_daas_config->env_name_magic,
						 hagen_daas_config->env_content_magic) != 1)
	{
		return HAGEN_DAAS_PLUGIN_FAILURE;
	}
	++job_desc->env_size;

	if (env_array_append(&job_desc->environment,
						 hagen_daas_config->env_name_scoop_ip,
						 scoop->ip) != 1)
	{
		return HAGEN_DAAS_PLUGIN_FAILURE;
	}
	++job_desc->env_size;

	if (env_array_append_fmt(&job_desc->environment, hagen_daas_config->env_name_scoop_port,
		"%d", scoop->service->port) != 1)
	{
		return HAGEN_DAAS_PLUGIN_FAILURE;
	}
	++job_desc->env_size;

	if (env_array_append(&job_desc->environment, hagen_daas_config->env_name_scoop_board_id,
			scoop->board_id) != 1)
	{
		return HAGEN_DAAS_PLUGIN_FAILURE;
	}
	++job_desc->env_size;

	if (_check_scoop_running(scoop) && _job_record_valid(scoop->job_record))
	{
		// if the scoop is already running, tell the spank plugin
		if (env_array_append_fmt(
				&job_desc->environment,
				hagen_daas_config->env_name_scoop_job_id,
				"%d",
				scoop->job_record->job_id) != 1)
		{
			return HAGEN_DAAS_PLUGIN_FAILURE;
		}
		++job_desc->env_size;
	}
	else
	{
		debug2("[hagen-daas] Scoop is not running yet -> requeue");
	}

    // TODO: DELME
	debug2("[hagen-daas] DUMP environment");
	size_t i;
	for (i=0; i<job_desc->env_size; ++i)
	{
		debug2("[hagen-daas] #%zu: %s", i, job_desc->environment[i]);
	}

	// we need to be able to requeue the job if scoop allocation fails
	job_desc->requeue = 1;

	return HAGEN_DAAS_PLUGIN_SUCCESS;
}

static service_t const* _get_service(char const* service_name)
{
	size_t i;
	for (i = 0; i < hagen_daas_config->num_services; ++i)
	{
		if (0 ==
			strcmp(service_name, hagen_daas_config->services[i].name))
		{
			return hagen_daas_config->services + i;
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
	option_entry_t* option_board_id = &parsed_options[_option_lookup("daas_board_id")];

	if (option_board_id->num_arguments > 1)
	{
		snprintf(
			function_error_msg, MAX_LENGTH_ERROR,
			"We currently support one experiment board per job only!");
		return HAGEN_DAAS_PLUGIN_FAILURE;
	}

	char* board_id = option_board_id->arguments[0];

	// first, see if the scoop is already running and get the information from
	// there note that we do not lock the mutex_running_scoops_l because there
	// cant be a race condition -> the node where the scoop is located does not
	// depend on whether it is running
	running_scoop_t* scoop = _board_id_to_scoop(board_id);
	bool scoop_running = scoop != NULL && _check_scoop_running(scoop);

	if (!scoop_running)
	{
		debug2("[hagen-daas] Scoop not running, setting up..");
		// if the scoop is not running, we have to look up where it would run so
		// that the job has these information
		service_t const* service = board_id_to_service(board_id);
		if (service == NULL )
		{
			xfree(scoop);
			snprintf(
				function_error_msg, MAX_LENGTH_ERROR,
				"No service defined for board-id %s!", board_id);
			return HAGEN_DAAS_PLUGIN_FAILURE;
		}

		struct node_record* node = _gres_to_node(board_id);

		// TODO DELME start
		if (node != NULL)
		{
			debug2("[hagen-daas] Found node %s hosting %s", node->node_hostname, board_id);
		}
		else
		{
			warn("[hagen-daas] Found no node hosting %s", board_id);
		}
		// TODO DELME stop

		if (node == NULL )
		{
			xfree(scoop);
			snprintf(
				function_error_msg, MAX_LENGTH_ERROR,
				"Specified board-id not found!");
			return HAGEN_DAAS_PLUGIN_FAILURE;
		}

		// build the mock scoop
		scoop = _build_scoop(board_id, node, service);
	}
	else
	{
		debug2("[hagen-daas] Scoop is already running, nothing to do..");
	}

	int retval = _modify_job_desc_compute_job(job_desc, scoop);
	if (!scoop_running)
	{
		// if the scoop is not running we built a temporary placeholder
		// -> free it
		xfree(scoop);
	}
	return retval;
}

static int _ensure_scoop_launched(
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
		&parsed_options[_option_lookup("launch_scoop")];

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

	if (scoop != NULL && !_check_scoop_running(scoop))
	{
		_remove_scoop_from_list(scoop); // scoop is freed there
		scoop = NULL;
	}

	bool scoop_already_running = true;

	// if scoop != NULL -> scoop is running (checked above) -> nothing to do
	if (scoop == NULL)
	{
		// if the scoop is not running, we have to look up where it would run so
		// that the job has these information
		service_t const* service = board_id_to_service(board_id);

		if (service == NULL)
		{
			error("[hagen-daas] Supplied board id not associated to any scoop job.");
			slurm_mutex_unlock(&mutex_running_scoops_l);
			return HAGEN_DAAS_PLUGIN_FAILURE;
		}

		struct node_record* node = _gres_to_node(board_id);

		scoop = _build_scoop(board_id, node, service);

		// if the controller was restarted in the meantime our list might not
		// contain the still running job -> check again
		_associate_scoop_job_record(scoop);

		// check again
		if (!_check_scoop_running(scoop))
		{
			// _launch_scoop_in_job_desc sets the job_id
			if ((_launch_scoop_in_job_desc(job_desc, scoop, node)
						!= HAGEN_DAAS_PLUGIN_SUCCESS))
			{
				slurm_mutex_unlock(&mutex_running_scoops_l);
				return HAGEN_DAAS_PLUGIN_FAILURE;
			}
			// only if we had to modify the job the scoop was not already
			// running
			scoop_already_running = false;
		}
		// in any case (newly started job or re-discovered job record) add the
		// scoop to the list
		//
		// set the apparent start time - please note that the start time is only
		// relevant if no corresponding job record can be found (short amount of
		// time after launch)
        scoop->t_start = time(NULL);
		list_append(running_scoops_l, scoop);
	}

	if (scoop_already_running)
	{
		{
			if (_job_record_valid(scoop->job_record))
			{
				debug("[hagen-daas] Scoop is already running in job #%d", scoop->job_record->job_id);
			}
			else
			{
				debug("[hagen-daas] Scoop is already running, but we do not have a job record yet!");
			}
		}
		slurm_mutex_unlock(&mutex_running_scoops_l);
		return HAGEN_DAAS_PLUGIN_NO_JOB_NEEDED;
	}

	slurm_mutex_unlock(&mutex_running_scoops_l);
	return HAGEN_DAAS_PLUGIN_SUCCESS;
}

static int _launch_scoop_in_job_desc(
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
	job_desc->script = _read_file((char*) scoop->service->script_path);

	job_desc->req_nodes = xstrdup(node->node_hostname);

	if (job_desc->name != NULL)
	{
		xfree(job_desc->name);
	}
	job_desc->name = _get_job_name(scoop);
	if (job_desc->name == NULL)
	{
		return HAGEN_DAAS_PLUGIN_FAILURE;
	}

	// accounting information
	struct passwd* pwd = getpwnam(hagen_daas_config->scoop_job_user); // no need to free
	if (pwd == NULL)
	{
		error("[hagen-daas] Failed to get uid/gid for hagen-daas user.");
		return HAGEN_DAAS_PLUGIN_FAILURE;
	}
	job_desc->user_id = pwd->pw_uid;
	job_desc->group_id = pwd->pw_gid;
	job_desc->account = xstrdup(scoop->service->slurm_account);
	job_desc->partition = xstrdup(scoop->service->slurm_partition);

	// resource information
	job_desc->cpus_per_task = scoop->service->num_cpus;
	job_desc->min_cpus = scoop->service->num_cpus;
	job_desc->pn_min_cpus = scoop->service->num_cpus;
	job_desc->pn_min_memory = scoop->service->memory_in_mb;
	job_desc->shared = 1;

	// reset job envionment
	job_desc->environment = env_array_create();
	job_desc->env_size = 0;

	// Inform the scoop via environment variables which board it will govern and
	// on which port it should listen.
	// NOTE: Job allocation WILL FAIL if not at least one environment variable
	// is set here!!
	if (env_array_append_fmt(&job_desc->environment,
							 hagen_daas_config->env_name_scoop_port,
							 "%d", scoop->service->port) != 1)
	{
		return HAGEN_DAAS_PLUGIN_FAILURE;
	}
	++job_desc->env_size;

	if (env_array_append(&job_desc->environment,
						 hagen_daas_config->env_name_scoop_board_id,
						 scoop->board_id) != 1)
	{
		return HAGEN_DAAS_PLUGIN_FAILURE;
	}
	++job_desc->env_size;

	char* old_work_dir = job_desc->work_dir;
	job_desc->work_dir = xstrdup(hagen_daas_config->scoop_working_dir);
	xfree(old_work_dir);

	// TODO: set further requirements if needed

	return HAGEN_DAAS_PLUGIN_SUCCESS;
}

static running_scoop_t* _build_scoop(
		char const* board_id,
		struct node_record const* node,
		service_t const* service)
{
	running_scoop_t* scoop = xmalloc(sizeof(running_scoop_t));

	strcpy(scoop->board_id, board_id);

	char* ip = _addr2ip(&node->slurm_addr);
	strcpy(scoop->ip, ip);
	xfree(ip);

	scoop->job_record = NULL;

	scoop->service = service;

	// a built scoop has not started yet -> no start time
	// will be set in _ensure_scoop_launched
	scoop->t_start = 0;

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
	// first method, try to find scoop in list
	running_scoop_t* scoop = list_find_first(
			l_running_scoops, _cmp_scoops_by_board_id, (void *) board_id);

	if (scoop != NULL)
	{
		_associate_scoop_job_record(scoop);
	}
	return scoop;
}

static bool _job_desc_is_batch_job(job_desc_msg_t* job_desc)
{
	// If submitted via srun the environment will be NULL
	// TODO find more reliable way to test this
	return (job_desc->environment != NULL);
}

static bool _check_scoop_running(running_scoop_t* scoop)
{
	xassert(scoop != NULL);

	// scoop might have just been launched -> avoid race condition
	if (scoop->job_record == NULL)
	{
		time_t now = time(NULL);
		return (now - scoop->t_start <=
				hagen_daas_config->scoop_launch_wait_secs);
	}

	// see if the magic cookie is still set -> pointer valid
	if (!_job_record_valid(scoop->job_record))
	{
		// job record is invalid
		scoop->job_record = NULL;
		return false;
	}

	if (IS_JOB_RUNNING(scoop->job_record) || IS_JOB_PENDING(scoop->job_record))
	{
		return true;
	}
	else
	{
		return false;
	}
}

static void _remove_scoop_from_list(running_scoop_t* scoop)
{
	debug("[hagen-daas] Removing scoop for board id %s.", scoop->board_id);
	ListIterator itr = list_iterator_create(l_running_scoops);
	struct running_scoop_t* tmp;
	while ((tmp = list_next(itr))) {
		// we are only interested in jobs run by slurm-daemon itself
		if ((void*) tmp == (void*) scoop)
		{
			break;
		}
	}
	if (tmp != NULL)
	{
		list_remove(itr);
		tmp = NULL;
	}
	else
	{
		error("[hagen-daas] Tried to remove non-existant scoop for board %s!",
				scoop->board_id);
	}
	list_iterator_destroy(itr);
}


static char* _read_file(char * const fname)
{
	int fd, i, offset = 0;
	struct stat stat_buf;
	char *file_buf;

	fd = open(fname, O_RDONLY);
	if (fd < 0) {
		fatal("[hagen-daas] Could not open scoop script %s: %m", fname);
	}
	if (fstat(fd, &stat_buf) < 0) {
		fatal("[hagen-daas] Could not stat scoop script %s: %m", fname);
	}
	file_buf = xmalloc(stat_buf.st_size);
	while (stat_buf.st_size > offset) {
		i = read(fd, file_buf + offset, stat_buf.st_size - offset);
		if (i < 0) {
			if (errno == EAGAIN)
				continue;
			fatal("[hagen-daas] Could not read scoop script file %s: %m", fname);
		}
		if (i == 0)
			break;	/* EOF */
		offset += i;
	}
	close(fd);
	return file_buf;
}

static char* _get_job_name(running_scoop_t* scoop)
{
	char* tmp = NULL;
	xstrcat(tmp, hagen_daas_config->scoop_jobname_prefix);
	xstrcat(tmp, scoop->board_id);
	return tmp;
}

static bool _check_job_name_for_board_id(
		char const* job_name, char const* board_id)
{
	size_t prefix_offset = strlen(hagen_daas_config->scoop_jobname_prefix);
	if (strlen(job_name) < (prefix_offset + strlen(board_id)))
	{
		return false;
	}
	else
	{
		return xstrcmp(job_name + prefix_offset, board_id) == 0;
	}
}


static struct job_record* _board_id_to_job_record(char const* board_id)
{
	// no need to free
	struct passwd* pwd = getpwnam(hagen_daas_config->scoop_job_user);
	if (pwd == NULL)
	{
		error("[hagen-daas] Failed to get uid/gid for hagen-daas user.");
		return NULL;
	}
	uint32_t job_uid = pwd->pw_uid;
	ListIterator itr = list_iterator_create(job_list);
	struct job_record* job;
	while ((job = list_next(itr))) {
		_dump_job_record(job);
		// we are only interested in jobs run by slurm-daemon itself
		if (job->user_id != job_uid)
		{
			continue;
		}
		// we only associate with jobs that are running or pending to run
        if (!(IS_JOB_RUNNING(job) || IS_JOB_PENDING(job)))
		{
			continue;
		}
		if (_check_job_name_for_board_id(job->name, board_id))
		{
			break;
		}
	}
	list_iterator_destroy(itr);
	return job;
}


static void _associate_scoop_job_record(running_scoop_t* scoop)
{
	if (scoop->job_record != NULL)
		return;

	scoop->job_record = _board_id_to_job_record(scoop->board_id);
}

static void _dump_scoop_list(void)
{
    ListIterator itr;
	size_t i = 0;
	int job_id;

	debug("[hagen-daas] Dumping scoop list contents:");
	itr = list_iterator_create(l_running_scoops);
	running_scoop_t* scoop;
	while ((scoop = list_next(itr))) {
		if (!_job_record_valid(scoop->job_record))
		{
			job_id = -1;
		}
		else
		{
			job_id = scoop->job_record->job_id;
		}
		debug("[hagen-daas] Scoop #%zu: %s for %s in job #%d",
			i, scoop->service->name, scoop->board_id, job_id);
		++i;
	}
	debug("[hagen-daas] Done dumping scoop list contents!");
	list_iterator_destroy(itr);
}

static void _dump_job_record(struct job_record const* job)
{
	debug("[hagen-daas] Dumping job record #%d", job->job_id);
	debug("[hagen-daas] [#%d] Jobnbame: %s", job->job_id, job->name);
	debug("[hagen-daas] [#%d] State: %d", job->job_id, job->job_state);
	debug("[hagen-daas] [#%d] UID: %d", job->job_id, job->user_id);
}

static bool _parsed_options_from_magic_env(
	job_desc_msg_t* job_desc,
	option_entry_t* parsed_options)
{
	if (job_desc->env_size == 0)
	{
		return false;
	}

	char* magic = getenvp(job_desc->environment,
						  hagen_daas_config->env_name_magic);
	if (xstrcmp(magic, hagen_daas_config->env_content_magic) != 0)
	{
		return false;
	}
	option_entry_t option = parsed_options[_option_lookup("launch_scoop")];

	strcpy(option.arguments[option.num_arguments],
			getenvp(job_desc->environment,
					hagen_daas_config->env_name_scoop_board_id));
	parsed_options[_option_lookup("launch_scoop")].num_arguments += 1;

	return true;
}

static bool _job_record_valid(struct job_record const* job)
{
	return job != NULL && job->magic == JOB_MAGIC;
}

// vim: sw=4 ts=4 sts=4 noexpandtab tw=80
