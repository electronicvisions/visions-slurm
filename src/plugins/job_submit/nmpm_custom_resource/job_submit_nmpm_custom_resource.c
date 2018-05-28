/*****************************************************************************\
 *  job_submit_nmpm_custom_resource.c - Manages hardware resources of the
 *  neuromorphic physical model platfrom via additional spank plugin options
\*****************************************************************************/

#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>

#include "hwdb4cpp/hwdb4c.h"
#include "slurm/slurm_errno.h"
#include "src/common/slurm_xlator.h"
#include "src/slurmctld/slurmctld.h"

#define SPANK_OPT_PREFIX "_SLURM_SPANK_OPTION_wafer_res_opts_"

#define NUM_FPGAS_ON_WAFER 48
#define NUM_HICANNS_ON_WAFER 384
#define MAX_ADCS_PER_WAFER 12
#define NUM_TRIGGER_PER_WAFER 12

#define MAX_NUM_ARGUMENTS NUM_HICANNS_ON_WAFER
#define MAX_ARGUMENT_CHAIN_LENGTH 10000 //max number of chars for one argument chain
#define MAX_ARGUMENT_LENGTH 50 //max number of chars for one element of an argument chain
#define MAX_ALLOCATED_MODULES 25
#define MAX_ERROR_LENGTH 5000
#define MAX_ADC_COORD_LENGTH 100
#define MAX_ENV_NAME_LENGTH 50
#define MAX_HICANN_ENV_LENGTH_PER_WAFER (6 * NUM_HICANNS_ON_WAFER + MAX_ENV_NAME_LENGTH)
#define MAX_ADC_ENV_LENGTH_PER_WAFER (MAX_ADC_COORD_LENGTH * MAX_ADCS_PER_WAFER + MAX_ENV_NAME_LENGTH)
#define MAX_LICENSE_STRING_LENGTH_PER_WAFER (MAX_ADC_COORD_LENGTH * MAX_ADCS_PER_WAFER + NUM_HICANNS_ON_WAFER * 6)
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
	char active_adcs[MAX_ADCS_PER_WAFER][MAX_ADC_COORD_LENGTH];
	bool active_trigger[NUM_TRIGGER_PER_WAFER];
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
#define NUM_OPTIONS 17
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
};

// global handle of hwdb
static struct hwdb4c_database_t* hwdb_handle = NULL;
// global string to hold error message for slurm
static char function_error_msg[MAX_ERROR_LENGTH];

static enum analog_out_mode {ONLY_AOUT0, ONLY_AOUT1, BOTH_AOUT};


/***********************\
* function declarations *
\***********************/

/* takes a string and converts if poossible to int and saves in ret
 * returns NMPM_PLUGIN_SUCCESS on success, NMPM_PLUGIN_FAILURE on failure */
static int _str2int(char const* str, int* ret);

/* takes a string and converts if poossible to unsigned int and saves in ret
 * returns NMPM_PLUGIN_SUCCESS on success, NMPM_PLUGIN_FAILURE if no
 * conversion possible or negative number */
static int _str2uint(char const* str, unsigned int* ret);

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
 * also sets correspondig fpga active
 * if aout is > -1 _add_adc will be called */
static int _add_hicann(size_t hicann_id, int aout, wafer_res_t* allocated_module);

/* checks if fpga and adc are in hwdb and adds ADC serial number to requested ADCs
 * valid aout values are 0/1 to get one of the two corresponding ADCs or 2 for both */
static int _add_adc(size_t fpga_id, int aout, wafer_res_t* allocated_module);

/* adds requested triggergroup of corresponding fpga */
static int _add_trigger(size_t fpga_id, wafer_res_t *allocated_module);

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
	wafer_res_t allocated_modules[MAX_ALLOCATED_MODULES]; //holds info which HICANNs, FPGAs and ADC were requeste
	size_t num_allocated_modules = 0; //track number of modules, used as index for allocated_modules
	char my_errmsg[MAX_ERROR_LENGTH]; //string for temporary error message
	char* hwdb_path = NULL;
	bool zero_res_args = true;
	bool wmod_only_hw_option = true;
	bool skip_master_alloc = false;
	bool without_trigger = false;
	bool at_least_one_adc_allocated = false;
	size_t counter;
	size_t modulecounter;
	char* slurm_licenses_string = NULL;
	char* slurm_licenses_environment_string = NULL;
	char* hicann_environment_string = NULL;
	char* adc_environment_string = NULL;
	int retval = SLURM_ERROR;

	// init variables
	for (optioncounter = 0; optioncounter < NUM_OPTIONS; optioncounter++) {
		parsed_options[optioncounter].num_arguments = 0;
	}
	strcpy(function_error_msg, "");

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

	//check if at least one wafer module given
	if (parsed_options[_option_lookup("wmod")].num_arguments == 0) {
		snprintf(my_errmsg, MAX_ERROR_LENGTH, "No wafer module given!");
		retval = ESLURM_INVALID_LICENSES;
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

	//analyze wmod argument
	for (argcount = 0; argcount < parsed_options[_option_lookup("wmod")].num_arguments; argcount++) {
		size_t wafer_id;
		bool valid_wafer;
		//get wafer ID
		if (_str2uint(parsed_options[_option_lookup("wmod")].arguments[argcount], (unsigned int*)&wafer_id) != NMPM_PLUGIN_SUCCESS) {
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
		}
		for (counter = 0; counter < NUM_HICANNS_ON_WAFER; counter++) {
			allocated_modules[num_allocated_modules].active_hicanns[counter] = false;
		}
		for (counter = 0; counter < NUM_TRIGGER_PER_WAFER; counter++) {
			allocated_modules[num_allocated_modules].active_trigger[counter] = false;
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
			if (_str2uint(parsed_options[_option_lookup("reticle_without_aout")].arguments[argcount], (unsigned int*)&reticle_id) != NMPM_PLUGIN_SUCCESS) {
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
			if (_str2uint(parsed_options[_option_lookup("fpga_without_aout")].arguments[argcount], (unsigned int*)&fpga_id) != NMPM_PLUGIN_SUCCESS) {
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
			if (_str2uint(parsed_options[_option_lookup("hicann_without_aout")].arguments[argcount], (unsigned int*)&hicann_id) != NMPM_PLUGIN_SUCCESS) {
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
			if (_str2uint(parsed_options[_option_lookup("reticle_of_hicann_without_aout")].arguments[argcount], (unsigned int*)&hicann_id) != NMPM_PLUGIN_SUCCESS) {
				snprintf(my_errmsg, MAX_ERROR_LENGTH, "Invalid reticle_of_hicann_without_aout argument %s", parsed_options[_option_lookup("reticle_of_hicann_without_aout")].arguments[argcount]);
				retval = ESLURM_INVALID_LICENSES;
				goto CLEANUP;
			}
			if (_add_fpga_of_hicann(hicann_id, -1, &allocated_modules[0]) != NMPM_PLUGIN_SUCCESS) {
				snprintf(my_errmsg, MAX_ERROR_LENGTH, "Adding Reticle of HICANN wihtou aout %s failed: %s", parsed_options[_option_lookup("reticle_of_hicann_without_aout")].arguments[argcount], function_error_msg);
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
			// add all FPGAs with all possbile ADCs
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
	slurm_licenses_string = malloc(sizeof(char) * num_allocated_modules * MAX_LICENSE_STRING_LENGTH_PER_WAFER);
	if(slurm_licenses_string == NULL) {
		snprintf(my_errmsg, MAX_ERROR_LENGTH, "Memory alloc for slurm license string failed");
		retval = SLURM_ERROR;
		goto CLEANUP;
	}
	strcpy(slurm_licenses_string, "");

	char slurm_licenses_env_name[] = "HARDWARE_LICENSES=";
	slurm_licenses_environment_string = malloc(sizeof(char) * (num_allocated_modules * MAX_LICENSE_STRING_LENGTH_PER_WAFER + strlen(slurm_licenses_env_name)));
	if(slurm_licenses_environment_string == NULL) {
		snprintf(my_errmsg, MAX_ERROR_LENGTH, "Memory alloc for slurm license string failed");
		retval = SLURM_ERROR;
		goto CLEANUP;
	}
	strcpy(slurm_licenses_environment_string, slurm_licenses_env_name);

	char hicann_env_name[] = "ALLOCATED_HICANNGLOBAL=";
	hicann_environment_string = malloc(sizeof(char) * (num_allocated_modules * MAX_HICANN_ENV_LENGTH_PER_WAFER + strlen(hicann_env_name)));
	if(hicann_environment_string == NULL) {
		snprintf(my_errmsg, MAX_ERROR_LENGTH, "Memory alloc for HICANN environment string failed");
		retval = SLURM_ERROR;
		goto CLEANUP;
	}
	strcpy(hicann_environment_string, hicann_env_name);

	char adc_env_name[] = "ALLOCATED_ADC=";
	adc_environment_string = malloc(sizeof(char) * (num_allocated_modules * MAX_ADC_ENV_LENGTH_PER_WAFER + strlen(adc_env_name)));
	if(adc_environment_string == NULL) {
		snprintf(my_errmsg, MAX_ERROR_LENGTH, "Memory alloc for ADC envrionment string failed");
		retval = SLURM_ERROR;
		goto CLEANUP;
	}
	strcpy(adc_environment_string, adc_env_name);


	for (modulecounter = 0; modulecounter < num_allocated_modules; modulecounter++) {
		char tempstring[MAX_ADC_COORD_LENGTH];
		size_t fpgacounter = 0;
		size_t num_active_fpgas = 0;
		size_t adccounter = 0;
		size_t hicanncounter = 0;
		size_t triggercounter = 0;

		if(!skip_master_alloc) {
			//check if more than one FPGA was requested, if true also request master FPGA
			for (fpgacounter = 0; fpgacounter < NUM_FPGAS_ON_WAFER; fpgacounter++) {
				if(allocated_modules[modulecounter].active_fpgas[fpgacounter]) {
					num_active_fpgas++;
				}
				if(num_active_fpgas > 1) {
					allocated_modules[modulecounter].active_fpgas[hwdb4c_master_FPGA_enum()] = true;
					break;
				}
			}
		}
		for (fpgacounter = 0; fpgacounter < NUM_FPGAS_ON_WAFER; fpgacounter++) {
			if (allocated_modules[modulecounter].active_fpgas[fpgacounter]) {
				snprintf(tempstring, MAX_ADC_COORD_LENGTH, "W%zuF%zu,", allocated_modules[modulecounter].wafer_id, fpgacounter );
				strcat(slurm_licenses_string, tempstring);
			}
		}
		for (hicanncounter = 0; hicanncounter < NUM_HICANNS_ON_WAFER; hicanncounter++) {
			if (allocated_modules[modulecounter].active_hicanns[hicanncounter]) {
				snprintf(tempstring, MAX_ADC_COORD_LENGTH, "%zu,", allocated_modules[modulecounter].wafer_id * NUM_HICANNS_ON_WAFER + hicanncounter );
				strcat(hicann_environment_string, tempstring);
			}
		}
		for (adccounter = 0; adccounter < allocated_modules[modulecounter].num_active_adcs; adccounter++) {
			snprintf(tempstring, MAX_ADC_COORD_LENGTH, "%s,", allocated_modules[modulecounter].active_adcs[adccounter] );
			strcat(slurm_licenses_string, tempstring);
			strcat(adc_environment_string, tempstring);
			at_least_one_adc_allocated= true;
		}
		if(without_trigger) {
			if(!at_least_one_adc_allocated) {
				snprintf(my_errmsg, MAX_ERROR_LENGTH, "--without trigger specified but no analog readout specified");
				retval = ESLURM_INVALID_LICENSES;
				goto CLEANUP;
			}
		} else {
			for (triggercounter = 0; triggercounter < NUM_TRIGGER_PER_WAFER; triggercounter++) {
				if (allocated_modules[modulecounter].active_trigger[triggercounter]) {
					snprintf(tempstring, MAX_ADC_COORD_LENGTH, "W%zuT%zu,", allocated_modules[modulecounter].wafer_id, triggercounter );
					strcat(slurm_licenses_string, tempstring);
				}
			}
		}
	}

	// delete trailing ','
	if (strlen(slurm_licenses_string) > 1) {
		slurm_licenses_string[strlen(slurm_licenses_string) - 1] = '\0';
	}
	strcat(slurm_licenses_environment_string, slurm_licenses_string);
	if(strlen(hicann_environment_string) > 1) {
		hicann_environment_string[strlen(hicann_environment_string) - 1] = '\0';
	}
	if(strlen(adc_environment_string) > 1) {
		adc_environment_string[strlen(adc_environment_string) - 1] = '\0';
	}

	xrealloc(job_desc->environment, sizeof(char *) * (job_desc->env_size + 3));
	job_desc->environment[job_desc->env_size] = xstrdup(hicann_environment_string);
	job_desc->environment[job_desc->env_size + 1] = xstrdup(adc_environment_string);
	job_desc->environment[job_desc->env_size + 2] = xstrdup(slurm_licenses_environment_string);
	job_desc->env_size = job_desc->env_size + 3;

	//set slurm licenses
	if(job_desc->licenses) {
		xrealloc(job_desc->licenses, sizeof(char *) * (strlen(slurm_licenses_string) + strlen(job_desc->licenses)));
		xstrcat(job_desc->licenses, slurm_licenses_string);
	} else {
		xrealloc(job_desc->licenses, sizeof(char *) * (strlen(slurm_licenses_string)));
		job_desc->licenses = xstrdup(slurm_licenses_string);
	}
	info("LICENSES: %s", job_desc->licenses);
	retval = SLURM_SUCCESS;

CLEANUP:

	if (retval != SLURM_SUCCESS) {
		*err_msg = xstrdup(my_errmsg);
		error("%s", my_errmsg);
	}
	if (slurm_licenses_string) {
		free(slurm_licenses_string);
	}
	if (slurm_licenses_environment_string) {
		free(slurm_licenses_environment_string);
	}
	if (hicann_environment_string) {
		free(hicann_environment_string);
	}
	if (adc_environment_string) {
		free(adc_environment_string);
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

static int _str2int (char const* str, int *p2int)
{
	long int value;
	char *end;

	if (str == NULL) {
		return NMPM_PLUGIN_FAILURE;
	}
	errno = 0;
	value = strtol (str, &end, 10);
	if (end == str || *end != '\0' || (errno == ERANGE && (value == LONG_MAX || value == LONG_MIN))) {
		return NMPM_PLUGIN_FAILURE;
	}
	if (value > INT_MAX || value < INT_MIN) {
		return NMPM_PLUGIN_FAILURE;
	}
	*p2int = (int) value;
	return NMPM_PLUGIN_SUCCESS;
}

static int _str2uint (char const* str, unsigned int *p2uint)
{
	int tmp;
	if (_str2int(str, &tmp) != NMPM_PLUGIN_SUCCESS) {
		return NMPM_PLUGIN_FAILURE;
	}
	if (tmp < 0) {
		return NMPM_PLUGIN_FAILURE;
	}
	*p2uint = (unsigned int)tmp;
	return NMPM_PLUGIN_SUCCESS;
}


static int _split_aout_arg(char const* arg, size_t *value, int *aout)
{
	char *aout_split;
	char *save_ptr;
	int tmp;
	if(strstr(arg, ":") == NULL) {
		if (_str2uint(arg, (unsigned int*)value) != NMPM_PLUGIN_SUCCESS)
			return NMPM_PLUGIN_FAILURE;
		*aout = BOTH_AOUT;
	}
	else {
		aout_split = strtok_r(arg, ":", &save_ptr);
		if (_str2uint(aout_split, (unsigned int*)value) != NMPM_PLUGIN_SUCCESS) {
			return NMPM_PLUGIN_FAILURE;
		}
		aout_split = strtok_r(NULL, ",", &save_ptr);
		if (_str2int(aout_split, &tmp) != NMPM_PLUGIN_SUCCESS) {
			return NMPM_PLUGIN_FAILURE;
		}
		if (tmp == 0) {
			*aout = ONLY_AOUT0;
		} else if (tmp == 1) {
			*aout = ONLY_AOUT1;
		} else {
			return NMPM_PLUGIN_FAILURE;
		}
	}
	return NMPM_PLUGIN_SUCCESS;
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
	char argumentsrc[MAX_ARGUMENT_CHAIN_LENGTH];
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
		spank_string = malloc(sizeof(char) * strlen(job_desc->spank_job_env[optioncount]));
		if (spank_string == 0) {
			snprintf(function_error_msg, MAX_ERROR_LENGTH, "spank_string memory alloc failed");
			retval = NMPM_PLUGIN_FAILURE;
			goto PARSE_OPTIONS_CLEANUP;
		}
		strcpy(spank_string, job_desc->spank_job_env[optioncount]);
		option = strstr(spank_string, SPANK_OPT_PREFIX);

		// some other spank option, skip
		if (option==NULL) {
			goto PARSE_OPTIONS_CLEANUP;
		}

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

		// truncate '=' at end of option string
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
			*zero_res_args = false;
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
			free(spank_string);
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
	if (hwdb4c_ReticleOnWafer_toFPGAOnWafer(reticle_id, &fpga_id) != HWDB4C_SUCCESS) {
		return NMPM_PLUGIN_FAILURE;
	}
	return _add_fpga(fpga_id, aout, allocated_module);
}

static int _add_fpga_of_hicann(size_t hicann_id, int aout, wafer_res_t *allocated_module)
{
	size_t fpga_id;
	if (hwdb4c_HICANNOnWafer_toFPGAOnWafer(hicann_id, &fpga_id) != HWDB4C_SUCCESS) {
		return NMPM_PLUGIN_FAILURE;
	}
	return _add_fpga(fpga_id, aout, allocated_module);
}

static int _add_fpga(size_t fpga_id, int aout, wafer_res_t *allocated_module)
{
	size_t hicanncounter;
	struct hwdb4c_hicann_entry** hicann_entries;
	size_t num_hicanns;
	bool has_fpga_entry;
	size_t global_fpga_id = allocated_module->wafer_id * NUM_FPGAS_ON_WAFER + fpga_id;

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
	if (aout > -1)
		if (_add_adc(fpga_id, aout, allocated_module) != NMPM_PLUGIN_SUCCESS)
			return NMPM_PLUGIN_FAILURE;
	return NMPM_PLUGIN_SUCCESS;
}

static int _add_hicann(size_t hicann_id, int aout, wafer_res_t *allocated_module)
{
	bool has_hicann_entry;
	bool has_fpga_entry;
	size_t fpga_id;

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
		if (_add_adc(fpga_id, aout, allocated_module) != NMPM_PLUGIN_SUCCESS)
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
	struct hwdb4c_adc_entry* adc_entry;
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
