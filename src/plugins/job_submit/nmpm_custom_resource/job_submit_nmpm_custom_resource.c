/*****************************************************************************\
 *  job_submit_nmpm_custom_resource.c - Manages hardware resources of the
 *  neuromorphic physical model platform via additional spank plugin options
 *
 *  This plugin has been developed by staff and students
 *  of Heidelberg University as part of the research carried out by the
 *  Electronic Vision(s) group at the Kirchhoff-Institute for Physics.
 *  The research is funded by Heidelberg University, the State of
 *  Baden-Württemberg, the Seventh Framework Programme under grant agreements
 *  no 604102 (HBP) as well as the Horizon 2020 Framework Programme under grant 
 *  agreement 720270 (HBP).
\*****************************************************************************/

#include <dirent.h>
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>

#include "hwdb4cpp/hwdb4c.h"
#include "slurm/slurm.h"
#include "slurm/slurm_errno.h"
#include "slurm/vision_defines.h"
#include "src/common/slurm_xlator.h"
#include "src/slurmctld/slurmctld.h"

#define SPANK_OPT_PREFIX "_SLURM_SPANK_OPTION_wafer_res_opts_"

#define NUM_FPGAS_ON_WAFER 48
#define NUM_HICANNS_ON_WAFER 384
#define MAX_ADCS_PER_WAFER 12
#define NUM_TRIGGER_PER_WAFER 12
#define NUM_ANANAS_PER_WAFER 2

#define MAX_NUM_ARGUMENTS NUM_HICANNS_ON_WAFER
#define MAX_ARGUMENT_CHAIN_LENGTH 10000 //max number of chars for one argument chain
#define MAX_ARGUMENT_LENGTH 50 //max number of chars for one element of an argument chain
#define MAX_ALLOCATED_MODULES 25
#define MAX_ERROR_LENGTH 5000
#define MAX_ADC_COORD_LENGTH 100
#define MAX_ENV_NAME_LENGTH 50
#define MAX_LICENSE_STRING_LENGTH 9 //WxxxHyyy,WxxxHyyy,....
#define MAX_HICANN_ENV_LENGTH_PER_WAFER (MAX_LICENSE_STRING_LENGTH * NUM_HICANNS_ON_WAFER + MAX_ENV_NAME_LENGTH)
#define MAX_ADC_ENV_LENGTH_PER_WAFER (MAX_ADC_COORD_LENGTH * MAX_ADCS_PER_WAFER + MAX_ENV_NAME_LENGTH)
#define MAX_LICENSE_STRING_LENGTH_PER_WAFER (MAX_ADC_COORD_LENGTH * MAX_ADCS_PER_WAFER + NUM_HICANNS_ON_WAFER * MAX_LICENSE_STRING_LENGTH + NUM_ANANAS_PER_WAFER * MAX_LICENSE_STRING_LENGTH)
#define NMPM_PLUGIN_SUCCESS 0
#define NMPM_PLUGIN_FAILURE -1
#define NMPM_MAGIC_BINARY_OPTION "praise the sun"


//SLURM plugin definitions
const char plugin_name[]        = "Job submit wafer resources plugin";
const char plugin_type[]        = "job_submit/nmpm_custom_resource";
const uint32_t plugin_version   = SLURM_VERSION_NUMBER;
const uint32_t min_plug_version = 100;

// holds information which resources are allocated for one wafer module
typedef struct wafer_res {
	size_t wafer_id;
	bool active_hicanns[NUM_HICANNS_ON_WAFER];
	bool active_fpgas[NUM_FPGAS_ON_WAFER];
	bool active_fpga_neighbor[NUM_FPGAS_ON_WAFER];
	char active_adcs[MAX_ADCS_PER_WAFER][MAX_ADC_COORD_LENGTH];
	bool active_trigger[NUM_TRIGGER_PER_WAFER];
	bool active_ananas[NUM_ANANAS_PER_WAFER];
	bool active_hicann_neighbor[NUM_HICANNS_ON_WAFER];
	size_t num_active_adcs;
} wafer_res_t;

// holds array of strings of one option entry
typedef struct option_entry {
	char arguments[MAX_NUM_ARGUMENTS][MAX_ARGUMENT_LENGTH];
	size_t num_arguments;
} option_entry_t;

// pair of option string and index
typedef struct option_index {
	char option_name[50];
	int index;
} option_index_t;

//global array of valid options
#define NUM_OPTIONS 21
// options that are only valid if single wafer option is given
#define WMOD_DEPENDENT_MIN_INDEX 4
#define WMOD_DEPENDENT_MAX_INDEX 11
static const option_index_t custom_res_options[NUM_OPTIONS] = {
	{ "wmod",                            0},
	{ "wafer",                           0},
	{ "hwdb_path",                       1},
	{ "skip_master_alloc",               2},
	{ "without_trigger",                 3},
	{ "reticle_with_aout",               4},
	{ "fpga_with_aout",                  5},
	{ "hicann_with_aout",                6},
	{ "reticle_of_hicann_with_aout",     7},
	{ "reticle",                         4},
	{ "fpga",                            5},
	{ "hicann",                          6},
	{ "reticle_of_hicann",               7},
	{ "reticle_without_aout",            8},
	{ "fpga_without_aout",               9},
	{ "hicann_without_aout",            10},
	{ "reticle_of_hicann_without_aout", 11},
	{ "skip_hicann_init",               12},
	{ "force_hicann_init",              13},
	{ "defects_path",                   14},
	{ "powercycle",                     15}
};

// global handle of hwdb
static struct hwdb4c_database_t* hwdb_handle = NULL;
// global string to hold error message for slurm
static char function_error_msg[MAX_ERROR_LENGTH] = "";

enum analog_out_mode {ONLY_AOUT0, ONLY_AOUT1, BOTH_AOUT};


/***********************\
* function declarations *
\***********************/

/* takes a string and converts if possible to long and saves in ret
 * returns NMPM_PLUGIN_SUCCESS on success, NMPM_PLUGIN_FAILURE on failure */
static int _str2l(char const* str, long* ret);

/* takes a string and converts if possible to unsigned long and saves in ret
 * returns NMPM_PLUGIN_SUCCESS on success, NMPM_PLUGIN_FAILURE if no
 * conversion possible or negative number */
static int _str2ul(char const* str, unsigned long* ret);

/* takes an option string and returns corresponding index, if string is no valid option returns
 * NMPM_PLUGIN_FAILURE */
static int _option_lookup(char const* option_string);

/* parses the options from the spank job environment given by job_desc and converts them to
 job_entries. zero_res_args is true if no spank options regarding nmpm resource management where found */
static int _parse_options(
	struct job_descriptor const* job_desc,
	option_entry_t* parsed_options,
	bool* zero_res_args);

/* takes string of a "-with-aout" option, and sets aout of either 0/1 when aout was specified via
 * colon delimiter
 *  or 2 if none was given, i.e. both aout should be requested */
static int _split_aout_arg(char const* arg, size_t* value, int* aout);

/* checks if FPGA is in hwdb and sets FPGA active in wafer_res_t
 * gets all HICANNs of fpga and sets also active
 * if aout is > -1 _add_adc will be called */
static int _add_fpga(size_t fpga_id, int aout, wafer_res_t* allocated_module);

/* converts Reticle to fpga and calls _add_fpga */
static int _add_reticle(size_t reticle_id, int aout, wafer_res_t* allocated_module);

/* converts HICANN to fpga and calls _add_fpga */
static int _add_fpga_of_hicann(size_t hicann_id, int aout, wafer_res_t* allocated_module);

/* checks if HICANN is in hwdb and sets HICANN active in wafer_res_t
 * also sets corresponding fpga active
 * if aout is > -1 _add_adc will be called */
static int _add_hicann(size_t hicann_id, int aout, wafer_res_t* allocated_module);

/* checks if fpga and adc are in hwdb and adds ADC serial number to requested ADCs
 * valid aout values are 0/1 to get one of the two corresponding ADCs or 2 for both */
static int _add_adc(size_t fpga_id, int aout, wafer_res_t* allocated_module);

/* adds requested trigger group of corresponding fpga */
static int _add_trigger(size_t fpga_id, wafer_res_t *allocated_module);

/* sets ananas of corresponding fpga active for allocated_module */
static int _add_ananas(size_t fpga_id, wafer_res_t *allocated_module);

/* Check if neighboring HICANNs exist and set those as active_hicann_neighbors
 * except if they are already active HICANNs. Same is done for corresponding FPGAs */
static int _add_neighbors(size_t hicann_id, wafer_res_t *allocated_module);

/* Helper function for _add_neighbors(). Checks if neighbor hicann exists and sets it active
 * if the neighboring hicann itself is not active. Same is done for the corresponding FPGA */
static int _allocate_neighbor(size_t hicann_id, wafer_res_t *allocated_module, int (*get_neighbor)(size_t, size_t*));

/* Extract information for powercycle script in prolog script. */
static int _get_powercycle_info(struct job_descriptor *job_desc, char **info);

/* Convert give id with provided to_slurm_license conversion function to license string and append to env_string. */
static int _append_slurm_license(size_t id, int (*to_slurm_license)(size_t, char**), char *env_string);

/***********************\
* function definitions *
\***********************/

//slurm required functions
int init (void) {return SLURM_SUCCESS;}
void fini (void) {}

//main plugin function
extern int job_submit(struct job_descriptor *job_desc, uint32_t submit_uid, char **err_msg)
{
	size_t optioncounter, argcount;
	option_entry_t parsed_options[NUM_OPTIONS]; // holds all parsed options
	//FIXME make allocated_modules dynamic size
	wafer_res_t allocated_modules[MAX_ALLOCATED_MODULES]; //holds info which HICANNs, FPGAs and ADC were requested
	size_t num_allocated_modules = 0; //track number of modules, used as index for allocated_modules
	char my_errmsg[MAX_ERROR_LENGTH]; //string for temporary error message
	char* hwdb_path = NULL;
	char* defects_path = NULL;
	bool zero_res_args = true;
	bool wmod_only_hw_option = true;
	bool skip_master_alloc = false;
	bool without_trigger = false;
	bool skip_hicann_init = false;
	bool force_hicann_init = false;
	bool powercycle = false;
	size_t counter;
	size_t modulecounter;
	char* slurm_licenses_string = NULL;
	char* slurm_licenses_environment_string = NULL;
	char* slurm_neighbor_hicanns_environment_string = NULL;
	char* slurm_neighbor_licenses_raw_string = NULL;
	char* slurm_neighbor_licenses_environment_string = NULL;
	char* slurm_defects_path_environment_string = NULL;
	char* slurm_hicann_init_env = NULL; // holds info about automated hicann init
	char* hicann_environment_string = NULL;
	char* adc_environment_string = NULL;
	int retval = SLURM_ERROR;

	if (job_desc->licenses) {
		snprintf(my_errmsg, MAX_ERROR_LENGTH, "Manual licenses not supported");
		retval = ESLURM_NOT_SUPPORTED;
		goto CLEANUP;
	}

	// init variables
	for (optioncounter = 0; optioncounter < NUM_OPTIONS; optioncounter++) {
		parsed_options[optioncounter].num_arguments = 0;
	}

	// get parsed options
	if (_parse_options(job_desc, parsed_options, &zero_res_args) != NMPM_PLUGIN_SUCCESS) {
		snprintf(my_errmsg, MAX_ERROR_LENGTH, "_parse_options: %s", function_error_msg);
		retval = SLURM_ERROR;
		goto CLEANUP;
	}

	//check if any res arg was given, if not exit successfully
	if (zero_res_args) {
		info("no custom vision resource options given");
		retval = SLURM_SUCCESS;
		goto CLEANUP;
	}

	//check if more modules are tried to be allocated than allowed
	if (parsed_options[_option_lookup("wmod")].num_arguments > MAX_ALLOCATED_MODULES) {
		snprintf(my_errmsg, MAX_ERROR_LENGTH,
		   "Requested to many wafer modules: %zu requested %d allowed",
		   parsed_options[_option_lookup("wmod")].num_arguments,
		   MAX_ALLOCATED_MODULES);
		retval = SLURM_ERROR;
		goto CLEANUP;
	}

	//check if wmod is only hw option given
	for (counter = WMOD_DEPENDENT_MIN_INDEX; counter <= WMOD_DEPENDENT_MAX_INDEX; counter++) {
		if (parsed_options[counter].num_arguments > 0) {
			wmod_only_hw_option = false;
		}
	}

	// alloc hwdb struct and load hwdb with either given or default path
	if (hwdb4c_alloc_hwdb(&hwdb_handle) != HWDB4C_SUCCESS) {
		snprintf(my_errmsg, MAX_ERROR_LENGTH, "HWDB alloc failed!");
		retval = SLURM_ERROR;
		goto CLEANUP;
	}
	if (parsed_options[_option_lookup("hwdb_path")].num_arguments == 1) {
		hwdb_path = parsed_options[_option_lookup("hwdb_path")].arguments[0];
	} else if (parsed_options[_option_lookup("hwdb_path")].num_arguments > 1) {
		snprintf(my_errmsg, MAX_ERROR_LENGTH, "multiple HWDB paths given!");
		retval = SLURM_ERROR;
		goto CLEANUP;
	}
	if (hwdb4c_load_hwdb(hwdb_handle, hwdb_path) != HWDB4C_SUCCESS) {
		snprintf(my_errmsg, MAX_ERROR_LENGTH, "HWDB load failed, maybe wrong path?");
		retval = SLURM_ERROR;
		goto CLEANUP;
	}

	if (parsed_options[_option_lookup("skip_master_alloc")].num_arguments == 1) {
		if (strcmp(parsed_options[_option_lookup("skip_master_alloc")].arguments[0], NMPM_MAGIC_BINARY_OPTION) != 0) {
			snprintf(my_errmsg, MAX_ERROR_LENGTH, "Invalid magic skip-master-alloc argument %s", parsed_options[_option_lookup("skip_master_alloc")].arguments[0]);
			retval = SLURM_ERROR;
			goto CLEANUP;
		}
		skip_master_alloc = true;
	}

	if (parsed_options[_option_lookup("without_trigger")].num_arguments == 1) {
		if (strcmp(parsed_options[_option_lookup("without_trigger")].arguments[0], NMPM_MAGIC_BINARY_OPTION) != 0) {
			snprintf(my_errmsg, MAX_ERROR_LENGTH, "Invalid magic without-trigger argument %s", parsed_options[_option_lookup("without_trigger")].arguments[0]);
			retval = SLURM_ERROR;
			goto CLEANUP;
		}
		without_trigger = true;
	}

	if (parsed_options[_option_lookup("skip_hicann_init")].num_arguments == 1) {
		if (strcmp(parsed_options[_option_lookup("skip_hicann_init")].arguments[0], NMPM_MAGIC_BINARY_OPTION) != 0) {
			snprintf(my_errmsg, MAX_ERROR_LENGTH, "Invalid magic skip_hicann_init argument %s", parsed_options[_option_lookup("skip_hicann_init")].arguments[0]);
			retval = SLURM_ERROR;
			goto CLEANUP;
		}
		skip_hicann_init = true;
	}

	if (parsed_options[_option_lookup("force_hicann_init")].num_arguments == 1) {
		if (strcmp(parsed_options[_option_lookup("force_hicann_init")].arguments[0], NMPM_MAGIC_BINARY_OPTION) != 0) {
			snprintf(my_errmsg, MAX_ERROR_LENGTH, "Invalid magic force_hicann_init argument %s", parsed_options[_option_lookup("force_hicann_init")].arguments[0]);
			retval = SLURM_ERROR;
			goto CLEANUP;
		}
		force_hicann_init = true;
	}

	if (parsed_options[_option_lookup("defects_path")].num_arguments == 1) {
		defects_path = parsed_options[_option_lookup("defects_path")].arguments[0];
		DIR* dir = opendir(defects_path);
		if (dir) {
		    closedir(dir);
		} else {
			switch(errno) {
				case ENOENT: snprintf(my_errmsg, MAX_ERROR_LENGTH, "Defects path \"%s\" does not exist", parsed_options[_option_lookup("defects_path")].arguments[0]);
				             retval = SLURM_ERROR;
				             goto CLEANUP;
				case EACCES: snprintf(my_errmsg, MAX_ERROR_LENGTH, "Defects path \"%s\" permission denied", parsed_options[_option_lookup("defects_path")].arguments[0]);
				             retval = SLURM_ERROR;
				             goto CLEANUP;
				case ENOTDIR: snprintf(my_errmsg, MAX_ERROR_LENGTH, "Defects path \"%s\" is file", parsed_options[_option_lookup("defects_path")].arguments[0]);
				              retval = SLURM_ERROR;
				              goto CLEANUP;
				default: snprintf(my_errmsg, MAX_ERROR_LENGTH, "Unexpected error while determine if defects path is valid: \"%s\"", parsed_options[_option_lookup("defects_path")].arguments[0]);
				         retval = SLURM_ERROR;
				         goto CLEANUP;
			}
		}
	} else if (parsed_options[_option_lookup("defects_path")].num_arguments > 1) {
		snprintf(my_errmsg, MAX_ERROR_LENGTH, "multiple (%d) defect paths given!", parsed_options[_option_lookup("defects_path")].num_arguments);
		retval = SLURM_ERROR;
		goto CLEANUP;
	}

	if (parsed_options[_option_lookup("powercycle")].num_arguments == 1) {
		if (strcmp(parsed_options[_option_lookup("powercycle")].arguments[0], NMPM_MAGIC_BINARY_OPTION) != 0) {
			snprintf(my_errmsg, MAX_ERROR_LENGTH, "Invalid magic powercycle argument %s", parsed_options[_option_lookup("powercycle")].arguments[0]);
			retval = SLURM_ERROR;
			goto CLEANUP;
		}
		powercycle = true;
	}

	// make sure that only one of force-hicann-init or skip-hicann-init options is passed
	if(skip_hicann_init && force_hicann_init) {
		snprintf(my_errmsg, MAX_ERROR_LENGTH, "Options '--force-hicann-init' and '--skip-hicann-init' are mutually exclusive");
		retval = SLURM_ERROR;
		goto CLEANUP;
	}

	//analyze wmod argument
	for (argcount = 0; argcount < parsed_options[_option_lookup("wmod")].num_arguments; argcount++) {
		size_t wafer_id;
		bool valid_wafer;
		//get wafer ID
		if (_str2ul(parsed_options[_option_lookup("wmod")].arguments[argcount], &wafer_id) != NMPM_PLUGIN_SUCCESS) {
			snprintf(my_errmsg, MAX_ERROR_LENGTH, "Invalid wmod argument %s", parsed_options[_option_lookup("wmod")].arguments[argcount]);
			retval = ESLURM_INVALID_LICENSES;
			goto CLEANUP;
		}
		//check if wafer in hwdb
		if (hwdb4c_has_wafer_entry(hwdb_handle, wafer_id, &valid_wafer) != HWDB4C_SUCCESS || !valid_wafer) {
			snprintf(my_errmsg, MAX_ERROR_LENGTH, "Wafer %zu not in hardware database", wafer_id);
			retval = ESLURM_INVALID_LICENSES;
			goto CLEANUP;
		}

		//check if wafer id already given
		for (counter = 0; counter < num_allocated_modules; counter++) {
			if (allocated_modules[counter].wafer_id == wafer_id) {
				snprintf(my_errmsg, MAX_ERROR_LENGTH, "Duplicate wafer module argument given %zu", wafer_id);
				retval = ESLURM_INVALID_LICENSES;
				goto CLEANUP;
			}
		}
		// initialize new module entry
		allocated_modules[num_allocated_modules].wafer_id = wafer_id;
		allocated_modules[num_allocated_modules].num_active_adcs = 0;
		for (counter = 0; counter < NUM_FPGAS_ON_WAFER; counter++) {
			allocated_modules[num_allocated_modules].active_fpgas[counter] = false;
			allocated_modules[num_allocated_modules].active_fpga_neighbor[counter] = false;
		}
		for (counter = 0; counter < NUM_HICANNS_ON_WAFER; counter++) {
			allocated_modules[num_allocated_modules].active_hicanns[counter] = false;
			allocated_modules[num_allocated_modules].active_hicann_neighbor[counter] = false;
		}
		for (counter = 0; counter < NUM_TRIGGER_PER_WAFER; counter++) {
			allocated_modules[num_allocated_modules].active_trigger[counter] = false;
		}
		for (counter = 0; counter < NUM_ANANAS_PER_WAFER; counter++) {
			allocated_modules[num_allocated_modules].active_ananas[counter] = false;
		}
		num_allocated_modules++;
	}

	if (num_allocated_modules > 1 && !wmod_only_hw_option) {
				snprintf(my_errmsg, MAX_ERROR_LENGTH, "multiple wafer modules given as well as additional options");
				retval = SLURM_ERROR;
				goto CLEANUP;
	}
	// look at other options if only one wafer module was specified and other resource arguments
	else if (!wmod_only_hw_option) {
		for (argcount = 0; argcount < parsed_options[_option_lookup("reticle_without_aout")].num_arguments; argcount++) {
			size_t reticle_id;
			if (_str2ul(parsed_options[_option_lookup("reticle_without_aout")].arguments[argcount], &reticle_id) != NMPM_PLUGIN_SUCCESS) {
				snprintf(my_errmsg, MAX_ERROR_LENGTH, "Invalid --reticle_without_aout argument %s", parsed_options[_option_lookup("reticle_without_aout")].arguments[argcount]);
				retval = ESLURM_INVALID_LICENSES;
				goto CLEANUP;
			}
			if (_add_reticle(reticle_id, -1, &allocated_modules[0]) != NMPM_PLUGIN_SUCCESS) {
				snprintf(my_errmsg, MAX_ERROR_LENGTH, "Adding reticle_without_aout %s failed: %s", parsed_options[_option_lookup("reticle_without_aout")].arguments[argcount], function_error_msg);
				retval = ESLURM_INVALID_LICENSES;
				goto CLEANUP;
			}
		}
		for (argcount = 0; argcount < parsed_options[_option_lookup("fpga_without_aout")].num_arguments; argcount++) {
			size_t fpga_id;
			if (_str2ul(parsed_options[_option_lookup("fpga_without_aout")].arguments[argcount], &fpga_id) != NMPM_PLUGIN_SUCCESS) {
				snprintf(my_errmsg, MAX_ERROR_LENGTH, "Invalid --fpga_without_aout argument %s", parsed_options[_option_lookup("fpga_without_aout")].arguments[argcount]);
				retval = ESLURM_INVALID_LICENSES;
				goto CLEANUP;
			}
			if (_add_fpga(fpga_id, -1, &allocated_modules[0]) != NMPM_PLUGIN_SUCCESS) {
				snprintf(my_errmsg, MAX_ERROR_LENGTH, "Adding fpga_without_aout %s failed: %s", parsed_options[_option_lookup("fpga_without_aout")].arguments[argcount], function_error_msg);
				retval = ESLURM_INVALID_LICENSES;
				goto CLEANUP;
			}
		}
		for (argcount = 0; argcount < parsed_options[_option_lookup("hicann_without_aout")].num_arguments; argcount++) {
			size_t hicann_id;
			if (_str2ul(parsed_options[_option_lookup("hicann_without_aout")].arguments[argcount], &hicann_id) != NMPM_PLUGIN_SUCCESS) {
				snprintf(my_errmsg, MAX_ERROR_LENGTH, "Invalid hicann_without_aout argument %s", parsed_options[_option_lookup("hicann_without_aout")].arguments[argcount]);
				retval = ESLURM_INVALID_LICENSES;
				goto CLEANUP;
			}
			if (_add_hicann(hicann_id, -1, &allocated_modules[0]) != NMPM_PLUGIN_SUCCESS) {
				snprintf(my_errmsg, MAX_ERROR_LENGTH, "Adding hicann_without_aout %s failed: %s", parsed_options[_option_lookup("hicann_without_aout")].arguments[argcount], function_error_msg);
				retval = ESLURM_INVALID_LICENSES;
				goto CLEANUP;
			}
		}
		for (argcount = 0; argcount < parsed_options[_option_lookup("reticle_of_hicann_without_aout")].num_arguments; argcount++) {
			size_t hicann_id;
			if (_str2ul(parsed_options[_option_lookup("reticle_of_hicann_without_aout")].arguments[argcount], &hicann_id) != NMPM_PLUGIN_SUCCESS) {
				snprintf(my_errmsg, MAX_ERROR_LENGTH, "Invalid reticle_of_hicann_without_aout argument %s", parsed_options[_option_lookup("reticle_of_hicann_without_aout")].arguments[argcount]);
				retval = ESLURM_INVALID_LICENSES;
				goto CLEANUP;
			}
			if (_add_fpga_of_hicann(hicann_id, -1, &allocated_modules[0]) != NMPM_PLUGIN_SUCCESS) {
				snprintf(my_errmsg, MAX_ERROR_LENGTH, "Adding Reticle of HICANN without aout %s failed: %s", parsed_options[_option_lookup("reticle_of_hicann_without_aout")].arguments[argcount], function_error_msg);
				retval = ESLURM_INVALID_LICENSES;
				goto CLEANUP;
			}
		}
		for (argcount = 0; argcount < parsed_options[_option_lookup("reticle_with_aout")].num_arguments; argcount++) {
			size_t reticle_id;
			int aout;
			if (_split_aout_arg(parsed_options[_option_lookup("reticle_with_aout")].arguments[argcount], &reticle_id, &aout) != NMPM_PLUGIN_SUCCESS) {
				snprintf(my_errmsg, MAX_ERROR_LENGTH, "Invalid --reticle_with_aout argument %s", parsed_options[_option_lookup("reticle_with_aout")].arguments[argcount]);
				retval = ESLURM_INVALID_LICENSES;
				goto CLEANUP;
			}
			if (_add_reticle(reticle_id, aout, &allocated_modules[0]) != NMPM_PLUGIN_SUCCESS) {
				snprintf(my_errmsg, MAX_ERROR_LENGTH, "Adding reticle-with-aout %s failed: %s", parsed_options[_option_lookup("reticle_with_aout")].arguments[argcount], function_error_msg);
				retval = ESLURM_INVALID_LICENSES;
				goto CLEANUP;
			}
		}
		for (argcount = 0; argcount < parsed_options[_option_lookup("fpga_with_aout")].num_arguments; argcount++) {
			size_t fpga_id;
			int aout;
			if (_split_aout_arg(parsed_options[_option_lookup("fpga_with_aout")].arguments[argcount], &fpga_id, &aout) != NMPM_PLUGIN_SUCCESS) {
				snprintf(my_errmsg, MAX_ERROR_LENGTH, "Invalid --fpga_with_aout argument %s", parsed_options[_option_lookup("fpga_with_aout")].arguments[argcount]);
				retval = ESLURM_INVALID_LICENSES;
				goto CLEANUP;
			}
			if (_add_fpga(fpga_id, aout, &allocated_modules[0]) != NMPM_PLUGIN_SUCCESS) {
				snprintf(my_errmsg, MAX_ERROR_LENGTH, "Adding fpga-with-aout %s failed: %s", parsed_options[_option_lookup("fpga_with_aout")].arguments[argcount], function_error_msg);
				retval = ESLURM_INVALID_LICENSES;
				goto CLEANUP;
			}
		}
		for (argcount = 0; argcount < parsed_options[_option_lookup("hicann_with_aout")].num_arguments; argcount++) {
			size_t hicann_id;
			int aout;
			if (_split_aout_arg(parsed_options[_option_lookup("hicann_with_aout")].arguments[argcount], &hicann_id, &aout) != NMPM_PLUGIN_SUCCESS) {
				snprintf(my_errmsg, MAX_ERROR_LENGTH, "Invalid --hicann_with_aout argument %s", parsed_options[_option_lookup("hicann_with_aout")].arguments[argcount]);
				retval = ESLURM_INVALID_LICENSES;
				goto CLEANUP;
			}
			if (_add_hicann(hicann_id, aout, &allocated_modules[0]) != NMPM_PLUGIN_SUCCESS) {
				snprintf(my_errmsg, MAX_ERROR_LENGTH, "Adding hicann-with-aout %s failed: %s", parsed_options[_option_lookup("hicann_with_aout")].arguments[argcount], function_error_msg);
				retval = ESLURM_INVALID_LICENSES;
				goto CLEANUP;
			}
		}
		for (argcount = 0; argcount < parsed_options[_option_lookup("reticle_of_hicann_with_aout")].num_arguments; argcount++) {
			size_t hicann_id;
			int aout;
			if (_split_aout_arg(parsed_options[_option_lookup("reticle_of_hicann_with_aout")].arguments[argcount], &hicann_id, &aout) != NMPM_PLUGIN_SUCCESS) {
				snprintf(my_errmsg, MAX_ERROR_LENGTH, "Invalid --reticle_of_hicann_with_aout argument %s", parsed_options[_option_lookup("reticle_of_hicann_with_aout")].arguments[argcount]);
				retval = ESLURM_INVALID_LICENSES;
				goto CLEANUP;
			}
			if (_add_fpga_of_hicann(hicann_id, aout, &allocated_modules[0]) != NMPM_PLUGIN_SUCCESS) {
				snprintf(my_errmsg, MAX_ERROR_LENGTH, "Adding reticle-of-hicann-with-aout %s failed: %s", parsed_options[_option_lookup("reticle_of_hicann_with_aout")].arguments[argcount], function_error_msg);
				retval = ESLURM_INVALID_LICENSES;
				goto CLEANUP;
			}
		}
	}
	// more than one module was given or only wmod option --> set all HICANNs and FPGAs and ADCs
	else {
		size_t modulecounter;
		size_t fpgacounter;
		bool has_fpga_entry;
		int aout;
		for (modulecounter = 0; modulecounter < num_allocated_modules; modulecounter++) {
			// add all FPGAs with all possible ADCs
			for (fpgacounter = 0; fpgacounter < NUM_FPGAS_ON_WAFER; fpgacounter++) {
				if(hwdb4c_has_fpga_entry(hwdb_handle, allocated_modules[modulecounter].wafer_id * NUM_FPGAS_ON_WAFER + fpgacounter, &has_fpga_entry) != HWDB4C_SUCCESS) {
					snprintf(my_errmsg, MAX_ERROR_LENGTH, "Adding whole Module %zu: FPGAOnWafer out of range %zu, this should never happen?!?", allocated_modules[modulecounter].wafer_id, fpgacounter);
					retval = ESLURM_INVALID_LICENSES;
					goto CLEANUP;
				}
				if (has_fpga_entry) {
					//check for both possible adcs
					bool has_adc0_entry, has_adc1_entry;
					if (hwdb4c_has_adc_entry(hwdb_handle, allocated_modules[modulecounter].wafer_id * NUM_FPGAS_ON_WAFER + fpgacounter, 0, &has_adc0_entry) != HWDB4C_SUCCESS) {
						snprintf(my_errmsg, MAX_ERROR_LENGTH, "FPGA %zu on Wafer-Module %zu has_adc for AnalogOnHICANN 0 failed", fpgacounter, allocated_modules[modulecounter].wafer_id);
						retval = ESLURM_INVALID_LICENSES;
						goto CLEANUP;
					}
					if (hwdb4c_has_adc_entry(hwdb_handle, allocated_modules[modulecounter].wafer_id * NUM_FPGAS_ON_WAFER + fpgacounter, 1, &has_adc1_entry) != HWDB4C_SUCCESS) {
						snprintf(my_errmsg, MAX_ERROR_LENGTH, "FPGA %zu on Wafer-Module %zu has_adc for AnalogOnHICANN 1 failed", fpgacounter, allocated_modules[modulecounter].wafer_id);
						retval = ESLURM_INVALID_LICENSES;
						goto CLEANUP;
					}
					// get combination of ADCS
					if (has_adc0_entry) {
						if (has_adc1_entry) {
							aout = 2;
						} else {
							aout = 0;
						}
					} else {
						if (has_adc1_entry) {
							aout = 1;
						} else {
							aout = -1;
						}
					}
					if (_add_fpga(fpgacounter, aout, &allocated_modules[modulecounter]) != NMPM_PLUGIN_SUCCESS) {
						snprintf(my_errmsg, MAX_ERROR_LENGTH, "Adding whole Module %zu: Adding FPGA %zu failed: %s", allocated_modules[modulecounter].wafer_id, fpgacounter, function_error_msg);
						retval = ESLURM_INVALID_LICENSES;
						goto CLEANUP;
					}
				}
			}
		}
	}

	//generate slurm license string from allocated modules
	slurm_licenses_string = xmalloc(num_allocated_modules * MAX_LICENSE_STRING_LENGTH_PER_WAFER + 1);
	strcpy(slurm_licenses_string, "");

	// add +2, one for '\0' and one for '='
	slurm_licenses_environment_string = xmalloc(num_allocated_modules * MAX_LICENSE_STRING_LENGTH_PER_WAFER + strlen(vision_slurm_hardware_licenses_env_name) + 2);
	strcpy(slurm_licenses_environment_string, vision_slurm_hardware_licenses_env_name);
	strcat(slurm_licenses_environment_string, "=");

	hicann_environment_string = xmalloc(num_allocated_modules * MAX_HICANN_ENV_LENGTH_PER_WAFER + strlen(vision_slurm_hicanns_env_name) + 2);
	strcpy(hicann_environment_string, vision_slurm_hicanns_env_name);
	strcat(hicann_environment_string, "=");

	adc_environment_string = xmalloc(num_allocated_modules * MAX_ADC_ENV_LENGTH_PER_WAFER + strlen(vision_slurm_adcs_env_name) + 2);
	strcpy(adc_environment_string, vision_slurm_adcs_env_name);
	strcat(adc_environment_string, "=");

	slurm_neighbor_licenses_raw_string = xmalloc(num_allocated_modules * MAX_LICENSE_STRING_LENGTH_PER_WAFER + 1);
	strcpy(slurm_neighbor_licenses_raw_string, "");

	slurm_neighbor_licenses_environment_string = xmalloc(num_allocated_modules * MAX_HICANN_ENV_LENGTH_PER_WAFER + strlen(vision_slurm_neighbor_licenses_env_name) + 2);
	strcpy(slurm_neighbor_licenses_environment_string, vision_slurm_neighbor_licenses_env_name);
	strcat(slurm_neighbor_licenses_environment_string, "=");

	slurm_neighbor_hicanns_environment_string = xmalloc(num_allocated_modules * MAX_HICANN_ENV_LENGTH_PER_WAFER + strlen(vision_slurm_neighbor_hicanns_env_name) + 2);
	strcpy(slurm_neighbor_hicanns_environment_string, vision_slurm_neighbor_hicanns_env_name);
	strcat(slurm_neighbor_hicanns_environment_string, "=");

	slurm_defects_path_environment_string = xmalloc(strlen(vision_slurm_defects_path_env_name) + 2);
	strcpy(slurm_defects_path_environment_string, vision_slurm_defects_path_env_name);
	strcat(slurm_defects_path_environment_string, "=");
	if (defects_path) {
		slurm_defects_path_environment_string = xrealloc(slurm_defects_path_environment_string, strlen(slurm_defects_path_environment_string) + strlen(defects_path) + 1);
		strcat(slurm_defects_path_environment_string, defects_path);
	}

	// add value to HICANN init env var, malloc for largets value, i.e. '=DEFAULT'
	slurm_hicann_init_env = xmalloc(strlen(vision_slurm_hicann_init_env_name) + (8 + 1));
	strcpy(slurm_hicann_init_env, vision_slurm_hicann_init_env_name);
	if(skip_hicann_init) {
		strcat(slurm_hicann_init_env, "=SKIP");
	} else if(force_hicann_init) {
		strcat(slurm_hicann_init_env, "=FORCE");
	} else {
		strcat(slurm_hicann_init_env, "=DEFAULT");
	}


	for (modulecounter = 0; modulecounter < num_allocated_modules; modulecounter++) {
		char tempstring[MAX_ADC_COORD_LENGTH];
		size_t fpgacounter = 0;
		size_t num_active_fpgas = 0;
		size_t adccounter = 0;
		size_t hicanncounter = 0;
		size_t triggercounter = 0;
		size_t ananascounter = 0;

		for (hicanncounter = 0; hicanncounter < NUM_HICANNS_ON_WAFER; hicanncounter++) {
			if (allocated_modules[modulecounter].active_hicanns[hicanncounter]) {
				size_t const global_id = allocated_modules[modulecounter].wafer_id * NUM_HICANNS_ON_WAFER + hicanncounter;
				if( _append_slurm_license(global_id, hwdb4c_HICANNGlobal_slurm_license, hicann_environment_string) != NMPM_PLUGIN_SUCCESS) {
					snprintf(my_errmsg, MAX_ERROR_LENGTH, "Creating slurm license for HICANN %lu failed", global_id);
					retval = SLURM_ERROR;
					goto CLEANUP;
				}
				// calculate neighbors
				if(!skip_hicann_init) {
					_add_neighbors(hicanncounter, &allocated_modules[modulecounter]);
				}
			}
		}
		// add neighbors to environment after we iterated over all active hicanns
		if(!skip_hicann_init) {
			for (hicanncounter = 0; hicanncounter < NUM_HICANNS_ON_WAFER; hicanncounter++) {
				if (allocated_modules[modulecounter].active_hicann_neighbor[hicanncounter]) {
					size_t const global_id = allocated_modules[modulecounter].wafer_id * NUM_HICANNS_ON_WAFER + hicanncounter;
					if( _append_slurm_license(global_id, hwdb4c_HICANNGlobal_slurm_license, slurm_neighbor_hicanns_environment_string) != NMPM_PLUGIN_SUCCESS) {
						snprintf(my_errmsg, MAX_ERROR_LENGTH, "Creating slurm license for HICANN %lu failed", global_id);
						retval = SLURM_ERROR;
						goto CLEANUP;
					}
				}
			}
			for (fpgacounter = 0; fpgacounter < NUM_FPGAS_ON_WAFER; fpgacounter++) {
				if (allocated_modules[modulecounter].active_fpga_neighbor[fpgacounter]) {
					size_t const global_id = allocated_modules[modulecounter].wafer_id * NUM_FPGAS_ON_WAFER + fpgacounter;
					if( _append_slurm_license(global_id, hwdb4c_FPGAGlobal_slurm_license, slurm_neighbor_licenses_raw_string) != NMPM_PLUGIN_SUCCESS) {
						snprintf(my_errmsg, MAX_ERROR_LENGTH, "Creating slurm license for FPGA %lu failed", global_id);
						retval = SLURM_ERROR;
						goto CLEANUP;
					}
				}
			}
		}
		if(!skip_master_alloc) {
			size_t const global_fpga_id = hwdb4c_master_FPGA_enum() + allocated_modules[modulecounter].wafer_id * NUM_FPGAS_ON_WAFER;
			bool has_fpga_entry = false;

			if ((hwdb4c_has_fpga_entry(hwdb_handle, global_fpga_id, &has_fpga_entry) == HWDB4C_SUCCESS) && has_fpga_entry) {
				//check if more than one FPGA was requested, if true also request master FPGA
				for (fpgacounter = 0; fpgacounter < NUM_FPGAS_ON_WAFER; fpgacounter++) {
					if(allocated_modules[modulecounter].active_fpgas[fpgacounter]) {
						num_active_fpgas++;
					}
					if(num_active_fpgas > 1) {
						allocated_modules[modulecounter].active_fpgas[hwdb4c_master_FPGA_enum()] = true;
						// more than one fpga found -> no more searching needed
						break;
					}
				}
			}
		}
		for (fpgacounter = 0; fpgacounter < NUM_FPGAS_ON_WAFER; fpgacounter++) {
			if (allocated_modules[modulecounter].active_fpgas[fpgacounter]) {
				size_t const global_id = allocated_modules[modulecounter].wafer_id * NUM_FPGAS_ON_WAFER + fpgacounter;
				if( _append_slurm_license(global_id, hwdb4c_FPGAGlobal_slurm_license, slurm_licenses_string) != NMPM_PLUGIN_SUCCESS) {
					snprintf(my_errmsg, MAX_ERROR_LENGTH, "Creating slurm license for FPGA %lu failed", global_id);
					retval = SLURM_ERROR;
					goto CLEANUP;
				}
			}
		}
		for (adccounter = 0; adccounter < allocated_modules[modulecounter].num_active_adcs; adccounter++) {
			snprintf(tempstring, MAX_ADC_COORD_LENGTH, "%s,", allocated_modules[modulecounter].active_adcs[adccounter] );
			strcat(slurm_licenses_string, tempstring);
			strcat(adc_environment_string, tempstring);
		}
		if(!without_trigger) {
			for (triggercounter = 0; triggercounter < NUM_TRIGGER_PER_WAFER; triggercounter++) {
				if (allocated_modules[modulecounter].active_trigger[triggercounter]) {
					size_t const global_id = allocated_modules[modulecounter].wafer_id * NUM_TRIGGER_PER_WAFER + triggercounter;
					if( _append_slurm_license(global_id, hwdb4c_TriggerGlobal_slurm_license, slurm_licenses_string) != NMPM_PLUGIN_SUCCESS) {
						snprintf(my_errmsg, MAX_ERROR_LENGTH, "Creating slurm license for Trigger %lu failed", global_id);
						retval = SLURM_ERROR;
						goto CLEANUP;
					}
				}
			}
			for (ananascounter = 0; ananascounter < NUM_ANANAS_PER_WAFER; ananascounter++) {
				if (allocated_modules[modulecounter].active_ananas[ananascounter]) {
					size_t const global_id = allocated_modules[modulecounter].wafer_id * NUM_ANANAS_PER_WAFER + ananascounter;
					if( _append_slurm_license(global_id, hwdb4c_ANANASGlobal_slurm_license, slurm_licenses_string) != NMPM_PLUGIN_SUCCESS) {
						snprintf(my_errmsg, MAX_ERROR_LENGTH, "Creating slurm license for ANANAS %lu failed", global_id);
						retval = SLURM_ERROR;
						goto CLEANUP;
					}
				}
			}
		}
	}

	// delete trailing ','
	if(strlen(slurm_licenses_string) > 1) {
		slurm_licenses_string[strlen(slurm_licenses_string) - 1] = '\0';
	}
	// first cat licenses to environment string that add neighbors to the requestes allocations, which are later removed in prolog script
	strcat(slurm_licenses_environment_string, slurm_licenses_string);
	if(strlen(hicann_environment_string) > strlen(hicann_environment_string) + 1) {
		hicann_environment_string[strlen(hicann_environment_string) - 1] = '\0';
	}
	if(strlen(slurm_neighbor_licenses_raw_string) > 1) {
		slurm_neighbor_licenses_raw_string[strlen(slurm_neighbor_licenses_raw_string) - 1] = '\0';
	}
	strcat(slurm_neighbor_licenses_environment_string, slurm_neighbor_licenses_raw_string);

	// add neighbor licenses to allocated license but only if not already present
	char *license_token = NULL;
	char *save_ptr = NULL;
	license_token = strtok_r(slurm_neighbor_licenses_raw_string, ",", &save_ptr);
	while(license_token != NULL) {
		if(strstr(slurm_licenses_string, license_token) == NULL) {
			strcat(slurm_licenses_string, ",");
			strcat(slurm_licenses_string, license_token);
		}
		license_token = strtok_r(NULL, ",", &save_ptr);
	}

	if(strlen(slurm_neighbor_hicanns_environment_string) > strlen(slurm_neighbor_hicanns_environment_string) + 1) {
		slurm_neighbor_hicanns_environment_string[strlen(slurm_neighbor_hicanns_environment_string) - 1] = '\0';
	}
	if(strlen(adc_environment_string) > strlen(vision_slurm_adcs_env_name) + 1) {
		adc_environment_string[strlen(adc_environment_string) - 1] = '\0';
	}

	xrealloc(job_desc->environment, sizeof(char *) * (job_desc->env_size + 6));
	job_desc->environment[job_desc->env_size] = xstrdup(hicann_environment_string);
	job_desc->environment[job_desc->env_size + 1] = xstrdup(adc_environment_string);
	job_desc->environment[job_desc->env_size + 2] = xstrdup(slurm_licenses_environment_string);
	job_desc->environment[job_desc->env_size + 3] = xstrdup(slurm_neighbor_licenses_environment_string);
	job_desc->environment[job_desc->env_size + 4] = xstrdup(slurm_hicann_init_env);
	job_desc->environment[job_desc->env_size + 5] = xstrdup(slurm_neighbor_hicanns_environment_string);
	job_desc->env_size += 6;
	//set slurm licenses (including neighbor licenses, those will be removed in prolog script)
	if(job_desc->licenses) {
		xrealloc(job_desc->licenses,strlen(slurm_licenses_string) + strlen(job_desc->licenses) + 1);
		xstrcat(job_desc->licenses, slurm_licenses_string);
	} else {
		job_desc->licenses = xstrdup(slurm_licenses_string);
	}
	char* powercycle_info = NULL;
	if(powercycle) {
		if (_get_powercycle_info(job_desc, &powercycle_info) != NMPM_PLUGIN_SUCCESS) {
			snprintf(my_errmsg, MAX_ERROR_LENGTH, "_get_powercycle_info: %s", function_error_msg);
			retval = SLURM_ERROR;
			goto CLEANUP;
		}
	}

	// write prolog relevant information into slurm admin comment
	size_t admin_comment_size = strlen(slurm_neighbor_licenses_environment_string) +
	                            strlen(slurm_hicann_init_env) +
	                            strlen(slurm_licenses_environment_string) +
	                            strlen(slurm_defects_path_environment_string) +
				    3 /* 3x;*/ + 1;
	if (job_desc->admin_comment) {
		admin_comment_size += strlen(job_desc->admin_comment);
		xrealloc(job_desc->admin_comment, admin_comment_size);
	} else {
		job_desc->admin_comment = xmalloc(admin_comment_size);
	}
	xstrcat(job_desc->admin_comment, slurm_neighbor_licenses_environment_string);
	xstrcat(job_desc->admin_comment, ";");
	xstrcat(job_desc->admin_comment, slurm_hicann_init_env);
	xstrcat(job_desc->admin_comment, ";");
	xstrcat(job_desc->admin_comment, slurm_licenses_environment_string);
	xstrcat(job_desc->admin_comment, ";");
	xstrcat(job_desc->admin_comment, slurm_defects_path_environment_string);
	if(powercycle_info) {
		xrealloc(job_desc->admin_comment, admin_comment_size + 1 + strlen(powercycle_info));
		xstrcat(job_desc->admin_comment, ";");
		xstrcat(job_desc->admin_comment, powercycle_info);
	}

	info("LICENSES: %s", job_desc->licenses);
	retval = SLURM_SUCCESS;

CLEANUP:

	if (retval != SLURM_SUCCESS) {
		*err_msg = xstrdup(my_errmsg);
		error("%s", my_errmsg);
	}
	if (slurm_licenses_string) {
		xfree(slurm_licenses_string);
		slurm_licenses_string = NULL;
	}
	if (slurm_licenses_environment_string) {
		xfree(slurm_licenses_environment_string);
		slurm_licenses_environment_string = NULL;
	}
	if (hicann_environment_string) {
		xfree(hicann_environment_string);
		hicann_environment_string = NULL;
	}
	if (slurm_neighbor_licenses_raw_string) {
		xfree(slurm_neighbor_licenses_raw_string);
	}
	if (slurm_neighbor_licenses_environment_string) {
		xfree(slurm_neighbor_licenses_environment_string);
	}
	if (slurm_neighbor_hicanns_environment_string) {
		xfree(slurm_neighbor_hicanns_environment_string);
	}
	if (slurm_hicann_init_env) {
		xfree(slurm_hicann_init_env);
	}
	if (adc_environment_string) {
		xfree(adc_environment_string);
		adc_environment_string = NULL;
	}
	if (powercycle_info) {
		xfree(powercycle_info);
		powercycle_info = NULL;
	}
	if (hwdb_handle) {
		hwdb4c_free_hwdb(hwdb_handle);
		hwdb_handle = NULL;
	}
	return retval;
}

extern int job_modify(struct job_descriptor *job_desc, struct job_record *job_ptr, uint32_t submit_uid)
{
	return SLURM_SUCCESS;
}

static int _str2l (char const* str, long *p2int)
{
	long value = 0;
	char *end = NULL;

	if (str == NULL) {
		return NMPM_PLUGIN_FAILURE;
	}
	errno = 0;
	value = strtol(str, &end, 10);
	if (end == str || *end != '\0' || (errno == ERANGE && (value == LONG_MAX || value == LONG_MIN))) {
		return NMPM_PLUGIN_FAILURE;
	}
	if (value > LONG_MAX || value < LONG_MIN) {
		return NMPM_PLUGIN_FAILURE;
	}
	*p2int = value;
	return NMPM_PLUGIN_SUCCESS;
}

static int _str2ul (char const* str, unsigned long *p2uint)
{
	unsigned long value = 0;
	char *end = NULL;

	if (str == NULL) {
		return NMPM_PLUGIN_FAILURE;
	}
	errno = 0;
	value = strtoul(str, &end, 10);
	if (end == str || *end != '\0' || (errno == ERANGE && (value == LONG_MAX || value == LONG_MIN))) {
		return NMPM_PLUGIN_FAILURE;
	}
	if (value > ULONG_MAX) {
		return NMPM_PLUGIN_FAILURE;
	}
	*p2uint = value;
	return NMPM_PLUGIN_SUCCESS;
}

static int _split_aout_arg(char const* carg, size_t *value, int *aout)
{
	char *aout_split = NULL;
	char *save_ptr = NULL;
	char *arg = NULL;
	long tmp;
	int retval = NMPM_PLUGIN_SUCCESS;
	arg = xstrdup(carg);
	if(strstr(arg, ":") == NULL) {
		if (_str2ul(arg, value) != NMPM_PLUGIN_SUCCESS) {
			retval = NMPM_PLUGIN_FAILURE;
			goto SPLIT_AOUT_ARG_CLEANUP;
		}
		*aout = BOTH_AOUT;
	}
	else {
		aout_split = strtok_r(arg, ":", &save_ptr);
		if (_str2ul(aout_split, value) != NMPM_PLUGIN_SUCCESS) {
			retval = NMPM_PLUGIN_FAILURE;
			goto SPLIT_AOUT_ARG_CLEANUP;
		}
		aout_split = strtok_r(NULL, ",", &save_ptr);
		if (_str2l(aout_split, &tmp) != NMPM_PLUGIN_SUCCESS) {
			retval = NMPM_PLUGIN_FAILURE;
			goto SPLIT_AOUT_ARG_CLEANUP;
		}
		if (tmp == 0) {
			*aout = ONLY_AOUT0;
		} else if (tmp == 1) {
			*aout = ONLY_AOUT1;
		} else {
			retval = NMPM_PLUGIN_FAILURE;
			goto SPLIT_AOUT_ARG_CLEANUP;
		}
	}
SPLIT_AOUT_ARG_CLEANUP:
	if (arg) {
		xfree(arg);
		arg = NULL;
	}
	return retval;
}

static int _option_lookup(char const *option_string)
{
	size_t indexcounter;
	for (indexcounter = 0; indexcounter < NUM_OPTIONS; indexcounter++) {
		if (strcmp(custom_res_options[indexcounter].option_name, option_string) == 0) {
			return custom_res_options[indexcounter].index;
		}
	}
	return NMPM_PLUGIN_FAILURE;
}

static int _parse_options(struct job_descriptor const *job_desc, option_entry_t *parsed_options, bool *zero_res_args)
{
	size_t optioncount, argcount;
	char argumentsrc[MAX_ARGUMENT_CHAIN_LENGTH] = {0};
	char *spank_string = NULL;
	char *arguments = NULL;
	char *option = NULL;
	char *argument_token = NULL;
	char *save_ptr = NULL;
	int retval = NMPM_PLUGIN_SUCCESS;

	// each option is formated the following way
	// _SLURM_SPANK_OPTION_wafer_res_opts_[option]=[argument,argument,...]
	// we iterate over all arguments of all options and save them in parsed_options
	for (optioncount = 0; optioncount < job_desc->spank_job_env_size; optioncount++) {
		spank_string = xmalloc(strlen(job_desc->spank_job_env[optioncount]) + 1);
		strcpy(spank_string, job_desc->spank_job_env[optioncount]);
		option = strstr(spank_string, SPANK_OPT_PREFIX);

		// some other spank option, skip
		if (option==NULL) {
			goto PARSE_OPTIONS_CLEANUP;
		}
		*zero_res_args = false;

		//truncate SPANK_OPT_PREFIX
		option += strlen(SPANK_OPT_PREFIX);
		strncpy(argumentsrc, option, MAX_ARGUMENT_CHAIN_LENGTH);
		//get string after = symbol
		arguments = strstr(argumentsrc, "=");
		if (arguments==NULL) {
			snprintf(function_error_msg, MAX_ERROR_LENGTH, "'=' not present in spank option string, this should never happen");
			retval = NMPM_PLUGIN_FAILURE;
			goto PARSE_OPTIONS_CLEANUP;
		}

		// truncate '=' at end of option string (replace = with 0)
		option[strlen(option) - strlen(arguments)] = 0;
		// truncate '=' at beginning of argument chain
		arguments += 1;

		if (_option_lookup(option) < 0) {
			snprintf(function_error_msg, MAX_ERROR_LENGTH, "Invalid option %s, please update spank arguments", option);
			retval = NMPM_PLUGIN_FAILURE;
			goto PARSE_OPTIONS_CLEANUP;
		}

		// options that don't need an argument have literal string "(null)" as argument
		// set them to magic string to check validity
		if (strcmp(arguments, "(null)") == 0) {
			strcpy(parsed_options[_option_lookup(option)].arguments[0], NMPM_MAGIC_BINARY_OPTION);
			parsed_options[_option_lookup(option)].num_arguments = 1;
		} else {
			if (strlen(arguments) > MAX_ARGUMENT_CHAIN_LENGTH) {
				snprintf(function_error_msg, MAX_ERROR_LENGTH, "To long argument, over %d chars", MAX_ARGUMENT_CHAIN_LENGTH);
				retval = NMPM_PLUGIN_FAILURE;
				goto PARSE_OPTIONS_CLEANUP;
			}
			argcount = parsed_options[_option_lookup(option)].num_arguments;
			argument_token = strtok_r(arguments, ",", &save_ptr);
			while(argument_token != NULL) {
				strcpy(parsed_options[_option_lookup(option)].arguments[argcount], argument_token);
				argcount++;
				parsed_options[_option_lookup(option)].num_arguments = argcount;
				argument_token = strtok_r(NULL, ",", &save_ptr);
			}
		}
PARSE_OPTIONS_CLEANUP:
		if (spank_string) {
			xfree(spank_string);
			spank_string = NULL;
		}
		if(retval == NMPM_PLUGIN_FAILURE) {
			break;
		}
	}
	return retval;
}

static int _add_reticle(size_t reticle_id, int aout, wafer_res_t *allocated_module)
{
	size_t fpga_id;

	// check if reticle_id in range
	if (reticle_id >= NUM_FPGAS_ON_WAFER) {
		snprintf(function_error_msg, MAX_ERROR_LENGTH, "Reticle %zu on Wafer-Module %zu out of range", reticle_id, allocated_module->wafer_id);
		return NMPM_PLUGIN_FAILURE;
	}

	if (hwdb4c_ReticleOnWafer_toFPGAOnWafer(reticle_id, &fpga_id) != HWDB4C_SUCCESS) {
		return NMPM_PLUGIN_FAILURE;
	}
	return _add_fpga(fpga_id, aout, allocated_module);
}

static int _add_fpga_of_hicann(size_t hicann_id, int aout, wafer_res_t *allocated_module)
{
	size_t fpga_id;

	// check if hicann_id in range
	if (hicann_id >= NUM_HICANNS_ON_WAFER) {
		snprintf(function_error_msg, MAX_ERROR_LENGTH, "HICANN %zu on Wafer-Module %zu out of range", hicann_id, allocated_module->wafer_id);
		return NMPM_PLUGIN_FAILURE;
	}

	if (hwdb4c_HICANNOnWafer_toFPGAOnWafer(hicann_id, &fpga_id) != HWDB4C_SUCCESS) {
		return NMPM_PLUGIN_FAILURE;
	}
	return _add_fpga(fpga_id, aout, allocated_module);
}

static int _add_fpga(size_t fpga_id, int aout, wafer_res_t *allocated_module)
{
	size_t hicanncounter;
	struct hwdb4c_hicann_entry** hicann_entries = NULL;
	size_t num_hicanns;
	bool has_fpga_entry;
	size_t global_fpga_id = allocated_module->wafer_id * NUM_FPGAS_ON_WAFER + fpga_id;

	// check if fpga_id in range
	if (fpga_id >= NUM_FPGAS_ON_WAFER) {
		snprintf(function_error_msg, MAX_ERROR_LENGTH, "FPGA %zu on Wafer-Module %zu out of range", fpga_id, allocated_module->wafer_id);
		return NMPM_PLUGIN_FAILURE;
	}

	// check if fpga is in hwdb_handle
	if (hwdb4c_has_fpga_entry(hwdb_handle, global_fpga_id, &has_fpga_entry) != HWDB4C_SUCCESS || !has_fpga_entry) {
		snprintf(function_error_msg, MAX_ERROR_LENGTH, "FPGA %zu on Wafer-Module %zu not in HWDB", fpga_id, allocated_module->wafer_id);
		return NMPM_PLUGIN_FAILURE;
	}

	if (hwdb4c_get_hicann_entries_of_FPGAGlobal(hwdb_handle, global_fpga_id, &hicann_entries, &num_hicanns) != HWDB4C_SUCCESS) {
		snprintf(function_error_msg, MAX_ERROR_LENGTH, "Failed to get HICANN entries for FPGA %zu on Wafer-Module %zu ", fpga_id, allocated_module->wafer_id);
		return NMPM_PLUGIN_FAILURE;
	}

	// add_hicanns
	for (hicanncounter = 0; hicanncounter < num_hicanns; hicanncounter++) {
		allocated_module->active_hicanns[ hicann_entries[hicanncounter]->hicannglobal_id % NUM_HICANNS_ON_WAFER ] = true;
	}

	hwdb4c_free_hicann_entries(hicann_entries, num_hicanns);

	allocated_module->active_fpgas[fpga_id] = true;
	if (aout > -1) {
		if (_add_adc(fpga_id, aout, allocated_module) != NMPM_PLUGIN_SUCCESS) {
			return NMPM_PLUGIN_FAILURE;
		}
	}
	if (_add_ananas(fpga_id, allocated_module) != NMPM_PLUGIN_SUCCESS) {
		return NMPM_PLUGIN_FAILURE;
	}
	return NMPM_PLUGIN_SUCCESS;
}

static int _add_hicann(size_t hicann_id, int aout, wafer_res_t *allocated_module)
{
	bool has_hicann_entry;
	bool has_fpga_entry;
	size_t fpga_id;

	// check if hicann_id in range
	if (hicann_id >= NUM_HICANNS_ON_WAFER) {
		snprintf(function_error_msg, MAX_ERROR_LENGTH, "HICANN %zu on Wafer-Module %zu out of range", hicann_id, allocated_module->wafer_id);
		return NMPM_PLUGIN_FAILURE;
	}

	// check if HICANN is in hwdb
	if(hwdb4c_has_hicann_entry(hwdb_handle, allocated_module->wafer_id * NUM_HICANNS_ON_WAFER + hicann_id, &has_hicann_entry) != HWDB4C_SUCCESS || !has_hicann_entry) {
		snprintf(function_error_msg, MAX_ERROR_LENGTH, "HICANN %zu on Wafer-Module %zu not in HWDB", hicann_id, allocated_module->wafer_id);
		return NMPM_PLUGIN_FAILURE;
	}
	if (hwdb4c_HICANNOnWafer_toFPGAOnWafer(hicann_id, &fpga_id) != HWDB4C_SUCCESS) {
		snprintf(function_error_msg, MAX_ERROR_LENGTH, "Failed to convert HICANN %zu on Wafer-Module %zu to FPGA", hicann_id, allocated_module->wafer_id);
		return NMPM_PLUGIN_FAILURE;
	}
	// check if FPGA is in hwdb
	if (hwdb4c_has_fpga_entry(hwdb_handle, allocated_module->wafer_id * NUM_FPGAS_ON_WAFER + fpga_id, &has_fpga_entry) != HWDB4C_SUCCESS || !has_fpga_entry) {
		snprintf(function_error_msg, MAX_ERROR_LENGTH, "FPGA %zu for HICANN %zu on Wafer-Module %zu not in HWDB",fpga_id, hicann_id, allocated_module->wafer_id);
		return NMPM_PLUGIN_FAILURE;
	}
	allocated_module->active_hicanns[hicann_id] = true;
	allocated_module->active_fpgas[fpga_id] = true;
	if (aout > -1) {
		if (_add_adc(fpga_id, aout, allocated_module) != NMPM_PLUGIN_SUCCESS) {
			return NMPM_PLUGIN_FAILURE;
		}
	}
	if (_add_ananas(fpga_id, allocated_module) != NMPM_PLUGIN_SUCCESS) {
		return NMPM_PLUGIN_FAILURE;
	}
	return NMPM_PLUGIN_SUCCESS;

}

static int _add_adc(size_t fpga_id, int aout, wafer_res_t *allocated_module)
{
	char adc_license[MAX_ADC_COORD_LENGTH];
	size_t adccounter;
	size_t aoutcounter;
	size_t aoutbegin;
	size_t aoutend;
	struct hwdb4c_adc_entry* adc_entry = NULL;
	size_t global_fpga_id = allocated_module->wafer_id * NUM_FPGAS_ON_WAFER + fpga_id;
	bool has_adc_entry;
	int retval = NMPM_PLUGIN_SUCCESS;

	switch(aout) {
		case ONLY_AOUT0:
			aoutbegin = 0;
			aoutend = 1;
			break;
		case ONLY_AOUT1:
			aoutbegin = 1;
			aoutend = 2;
			break;
		case BOTH_AOUT:
			aoutbegin = 0;
			aoutend = 2;
			break;
		default:
			snprintf(function_error_msg, MAX_ERROR_LENGTH, "AnalogOnHICANN %d out of range", aout);
			return NMPM_PLUGIN_FAILURE;
	}

	for (aoutcounter = aoutbegin; aoutcounter < aoutend; aoutcounter++) {
		if (hwdb4c_has_adc_entry(hwdb_handle, global_fpga_id, aoutcounter, &has_adc_entry) != HWDB4C_SUCCESS && !has_adc_entry) {
			snprintf(function_error_msg, MAX_ERROR_LENGTH, "ADC Entry (FPGAGlobal %zu, AnalogOnHICANN %zu) not in HWDB", global_fpga_id, aoutcounter);
			retval = NMPM_PLUGIN_FAILURE;
			goto ADD_ADC_CLEANUP;
		}
		if (hwdb4c_get_adc_entry(hwdb_handle, global_fpga_id, aoutcounter, &adc_entry) != HWDB4C_SUCCESS) {
			snprintf(function_error_msg, MAX_ERROR_LENGTH, "get ADC Entry (FPGAGlobal %zu, AnalogOnHICANN %zu) failed", global_fpga_id, aoutcounter);
			retval = NMPM_PLUGIN_FAILURE;
			goto ADD_ADC_CLEANUP;
		}
		if (_add_trigger(fpga_id, allocated_module) != NMPM_PLUGIN_SUCCESS) {
			snprintf(function_error_msg, MAX_ERROR_LENGTH, "failed to request trigger for (Wmod %zu)", allocated_module->wafer_id);
			retval = NMPM_PLUGIN_FAILURE;
			goto ADD_ADC_CLEANUP;
		}
		strncpy(adc_license, adc_entry->coord, MAX_ADC_COORD_LENGTH);
		// check if license is already requested
		for (adccounter = 0; adccounter < allocated_module->num_active_adcs; adccounter++) {
			if (strcmp(adc_license, allocated_module->active_adcs[adccounter]) == 0) {
				//license already in list of to be requested licenses
				goto ADD_ADC_CLEANUP;
			}
		}
		// check if requesting to many adcs
		if (allocated_module->num_active_adcs + 1 > MAX_ADCS_PER_WAFER) {
			snprintf(function_error_msg, MAX_ERROR_LENGTH, "Requesting more ADC licenses than available on one module (Wmod %zu)", allocated_module->wafer_id);
			retval = NMPM_PLUGIN_FAILURE;
			goto ADD_ADC_CLEANUP;
		}
		strcpy(allocated_module->active_adcs[allocated_module->num_active_adcs], adc_license);
		allocated_module->num_active_adcs++;

ADD_ADC_CLEANUP:
		if(adc_entry) {
			hwdb4c_free_adc_entry(adc_entry);
			adc_entry = NULL;
		}
		if (retval == NMPM_PLUGIN_FAILURE) {
			break;
		}
	}

	return retval;
}

static int _add_trigger(size_t fpga_id, wafer_res_t *allocated_module)
{
	size_t trigger_id = 0;
	if (hwdb4c_FPGAOnWafer_toTriggerOnWafer(fpga_id, &trigger_id) != HWDB4C_SUCCESS) {
		snprintf(function_error_msg, MAX_ERROR_LENGTH, "Conversion FPGAOnWafer %zu to TriggerOnWafer failed", fpga_id);
		return NMPM_PLUGIN_FAILURE;
	}
	allocated_module->active_trigger[trigger_id] = true;
	return NMPM_PLUGIN_SUCCESS;
}

static int _add_ananas(size_t fpga_id, wafer_res_t *allocated_module)
{
	size_t trigger_id = 0;
	size_t ananas_id = 0;
	bool has_ananas = false;
	if (hwdb4c_FPGAOnWafer_toTriggerOnWafer(fpga_id, &trigger_id) != HWDB4C_SUCCESS) {
		snprintf(function_error_msg, MAX_ERROR_LENGTH, "Conversion FPGAOnWafer %zu to TriggerOnWafer failed", fpga_id);
		return NMPM_PLUGIN_FAILURE;
	}
	if (hwdb4c_TriggerOnWafer_toANANASOnWafer(trigger_id, &ananas_id) != HWDB4C_SUCCESS) {
		snprintf(function_error_msg, MAX_ERROR_LENGTH, "Conversion TriggerOnWafer %zu to ANANASOnWafer failed", trigger_id);
		return NMPM_PLUGIN_FAILURE;
	}
	size_t global_ananas_id = allocated_module->wafer_id * NUM_ANANAS_PER_WAFER + ananas_id;
	if (hwdb4c_has_ananas_entry(hwdb_handle, global_ananas_id, &has_ananas) != HWDB4C_SUCCESS) {
		snprintf(function_error_msg, MAX_ERROR_LENGTH, "HWDB lookup of ANANASGlobal %zu failed", global_ananas_id);
		return NMPM_PLUGIN_FAILURE;
	}
	if (has_ananas) {
		allocated_module->active_ananas[ananas_id] = true;
	}
	return NMPM_PLUGIN_SUCCESS;
}

static int _add_neighbors(size_t hicann_id, wafer_res_t *allocated_module)
{
	size_t fpga_id;

	if (_allocate_neighbor(hicann_id, allocated_module, hwdb4c_HICANNOnWafer_east) != NMPM_PLUGIN_SUCCESS) {
		return NMPM_PLUGIN_FAILURE;
	}
	if (_allocate_neighbor(hicann_id, allocated_module, hwdb4c_HICANNOnWafer_south) != NMPM_PLUGIN_SUCCESS) {
		return NMPM_PLUGIN_FAILURE;
	}
	if (_allocate_neighbor(hicann_id, allocated_module, hwdb4c_HICANNOnWafer_west) != NMPM_PLUGIN_SUCCESS) {
		return NMPM_PLUGIN_FAILURE;
	}
	if (_allocate_neighbor(hicann_id, allocated_module, hwdb4c_HICANNOnWafer_north) != NMPM_PLUGIN_SUCCESS) {
		return NMPM_PLUGIN_FAILURE;
	}

	/* Since this HICANN is used by the experiment itself, it cannot be a neighbor,
	 * even if a neighbor-check from a previous HICANN already marked it as such */
	allocated_module->active_hicann_neighbor[hicann_id] = false;
	if (hwdb4c_HICANNOnWafer_toFPGAOnWafer(hicann_id, &fpga_id) != NMPM_PLUGIN_SUCCESS) {
		return NMPM_PLUGIN_FAILURE;
	}
	allocated_module->active_fpga_neighbor[fpga_id] = false;
	return NMPM_PLUGIN_SUCCESS;
}

static int _allocate_neighbor(size_t hicann_id,
                        wafer_res_t *allocated_module,
                        int (*get_neighbor)(size_t, size_t*))
{
	size_t hicann_neighbor_id;
	size_t fpga_id;
	bool has_fpga_entry = false;
	if (get_neighbor(hicann_id, &hicann_neighbor_id) == HWDB4C_SUCCESS) {
		if (hwdb4c_HICANNOnWafer_toFPGAOnWafer(hicann_neighbor_id, &fpga_id) != HWDB4C_SUCCESS) {
			return NMPM_PLUGIN_FAILURE;
		}
		if (hwdb4c_has_fpga_entry(hwdb_handle, allocated_module->wafer_id * NUM_FPGAS_ON_WAFER + fpga_id, &has_fpga_entry) != HWDB4C_SUCCESS) {
			return NMPM_PLUGIN_FAILURE;
		}
		// if no fpga in hwdb nothing to do
		if (!has_fpga_entry) {
			return NMPM_PLUGIN_SUCCESS;
		}
		if (!allocated_module->active_hicanns[hicann_neighbor_id]) {
			allocated_module->active_hicann_neighbor[hicann_neighbor_id] = true;
		}
		if (!allocated_module->active_fpgas[fpga_id]) {
			allocated_module->active_fpga_neighbor[fpga_id] = true;
		}
	}
	return NMPM_PLUGIN_SUCCESS;
}

static int _get_powercycle_info(struct job_descriptor *job_desc, char **return_info)
{
	if (*return_info) {
		snprintf(function_error_msg, MAX_ERROR_LENGTH, "Given pointer non-null");
		return NMPM_PLUGIN_FAILURE;
	}
	if (!job_desc->gres) {
		snprintf(function_error_msg, MAX_ERROR_LENGTH, "Powercycle requested but no gres given");
		return NMPM_PLUGIN_FAILURE;
	}

	// overall goal is to get ip and slot of network poweroutlet
	// * get all dls setups
	// * search gres in list of dls setups
	// * if exists extract information and write into char
	char** dls_setup_ids = NULL;
	size_t dls_setup_ids_size;
	size_t i;
	if (hwdb4c_get_dls_setup_ids(hwdb_handle, &dls_setup_ids, &dls_setup_ids_size) != HWDB4C_SUCCESS) {
		snprintf(function_error_msg, MAX_ERROR_LENGTH, "Could not get DLS setup IDs");
		return NMPM_PLUGIN_FAILURE;
	}
	for (i = 0; i < dls_setup_ids_size; i++) {
		if (strstr(dls_setup_ids[i], job_desc->gres)) {
			struct hwdb4c_dls_setup_entry *dls;
			if( hwdb4c_get_dls_entry(hwdb_handle, dls_setup_ids[i], &dls) != HWDB4C_SUCCESS) {
				snprintf(function_error_msg, MAX_ERROR_LENGTH, "Failed to aquire DLS setup entry %s", dls_setup_ids[i]);
				return NMPM_PLUGIN_FAILURE;
			}
			if(strcmp(dls->ntpwr_ip, " ") == 0) {
				snprintf(function_error_msg, MAX_ERROR_LENGTH, "Setup %s cannot be powercycled via ethernet", dls_setup_ids[i]);
				return NMPM_PLUGIN_FAILURE;
			}
			*return_info = xmalloc(strlen(vision_slurm_powercycle_env_name) +
			                       strlen(dls->ntpwr_ip) +
			                       2 /*, and '\0'*/ +
			                       21 /*ntpwr_slot is size_t so max 21 chars to represent number*/);
			sprintf(*return_info, "%s%s,%lu", vision_slurm_powercycle_env_name, dls->ntpwr_ip, dls->ntpwr_slot);
			hwdb4c_free_dls_setup_entry(dls);
		}
	}
	return NMPM_PLUGIN_SUCCESS;
}

static int _append_slurm_license(size_t id,
                        int (*to_slurm_license)(size_t, char**),
                        char* env_string)
{
	char* license_string = NULL;
	if( to_slurm_license(id, &license_string) != HWDB4C_SUCCESS) {
		return NMPM_PLUGIN_FAILURE;
	}
	strcat(env_string, license_string);
	strcat(env_string, ",");
	free(license_string);
	return NMPM_PLUGIN_SUCCESS;
}
