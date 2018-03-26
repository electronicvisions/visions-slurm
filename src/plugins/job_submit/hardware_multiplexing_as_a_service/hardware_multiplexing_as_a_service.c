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

#include "slurm/slurm_errno.h"
#include "src/common/slurm_xlator.h"
#include "src/slurmctld/slurmctld.h"

#define SPANK_OPT_PREFIX "_SLURM_SPANK_OPTION_hmaas_opts_"

#define NUM_FPGAS_ON_WAFER 48
#define NUM_HICANNS_ON_WAFER 384
#define MAX_ADCS_PER_WAFER 12

#define MAX_NUM_ARGUMENTS 1
#define MAX_ARGUMENT_CHAIN_LENGTH 10000 // max number of chars for one argument chain
#define MAX_ARGUMENT_LENGTH 50          // max number of chars for one element of an argument chain
#define MAX_ERROR_LENGTH 5000
#define MAX_ENV_NAME_LENGTH 50


// SLURM plugin definitions
const char plugin_name[] = "Job submit hardware multiplexing as a service plugin";
const char plugin_type[] = "job_submit/hmaas";
const uint32_t plugin_version = SLURM_VERSION_NUMBER;
const uint32_t min_plug_version = 100;

// holds array of strings of one option entry
struct option_entry
{
	char arguments[MAX_NUM_ARGUMENTS][MAX_ARGUMENT_LENGTH];
	size_t num_arguments;
};

// pair of option string and index
struct option_index_t
{
	char option_name[50];
	int index;
};

// global array of valid options
#define NUM_OPTIONS 2
static const struct option_index_t custom_res_options[NUM_OPTIONS] = {
	{ "hbid",           0},
	{ "hmaas-board-id", 0},
};

// global string to hold error message for slurm
static char function_error_msg[MAX_ERROR_LENGTH];


/***********************\
* function declarations *
\***********************/

/* takes a string and converts if poossible to int and saves in ret
 * returns NMPM_PLUGIN_SUCCESS on success, NMPM_PLUGIN_FAILURE on failure */
static int _str2int(char const* str, int* ret);

/* takes an option string and returns corresponding index, if string is no valid option returns
 * NMPM_PLUGIN_FAILURE */
static int _option_lookup(char const* option_string);

/* parses the options from the spank job environment given by job_desc and converts them to
 job_entries. zero_res_args is true if no spank options regarding nmpm resource management where
 found */
static int _parse_options(
	struct job_descriptor const* job_desc,
	struct option_entry* parsed_options,
	bool* zero_res_args);

/* takes string of a "-with-aout" option, and sets aout of either 0/1 when aout was specified via
 * colon delimiter
 *  or 2 if none was given, i.e. both aout should be requested */
static int _split_aout_arg(char const* arg, size_t* value, int* aout);

/* checks if FPGA is in hwdb and sets FPGA active in wafer_res_t
 * gets all HICANNs of fpga and sets also active
 * if aout is > -1 _add_adc will be called */
static int _add_fpga(size_t fpga_id, int aout, struct wafer_res_t* allocated_module);

/* converts Reticle to fpga and calls _add_fpga */
static int _add_reticle(size_t reticle_id, int aout, struct wafer_res_t* allocated_module);

/* converts HICANN to fpga and calls _add_fpga */
static int _add_fpga_of_hicann(size_t hicann_id, int aout, struct wafer_res_t* allocated_module);

/* checks if HICANN is in hwdb and sets HICANN active in wafer_res_t
 * also sets correspondig fpga active
 * if aout is > -1 _add_adc will be called */
static int _add_hicann(size_t hicann_id, int aout, struct wafer_res_t* allocated_module);

/* checks if fpga and adc are in hwdb and adds ADC serial number to requested ADCs
 * valid aout values are 0/1 to get one of the two corresponding ADCs or 2 for both */
static int _add_adc(size_t fpga_id, int aout, struct wafer_res_t* allocated_module);

/***********************\
* function definition *
\***********************/

// slurm required functions
int init(void)
{
	return SLURM_SUCCESS;
}
void fini(void) {}

// main plugin function
extern int job_submit(struct job_descriptor* job_desc, uint32_t submit_uid, char** err_msg)
{
	size_t optioncounter, argcount;
	struct option_entry parsed_options[NUM_OPTIONS]; // holds all parsed options
	// FIXME make allocated_modules dynamic size
	struct wafer_res_t allocated_modules[MAX_ALLOCATED_MODULES]; // holds info which HICANNs, FPGAs
																 // and ADC were requeste
	size_t num_allocated_modules = 0; // track number of modules, used as index for
									  // allocated_modules
	char my_errmsg[MAX_ERROR_LENGTH]; // string for temporary error message
	char* hwdb_path = NULL;
	bool zero_res_args = true;
	bool only_wmod_option = true;
	bool skip_master_alloc = false;
	size_t counter;
	size_t modulecounter;
	char* slurm_licenses_string = NULL;
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

	// check if any res arg was given, if not exit successfully
	if (zero_res_args) {
		info("no custom vision resource options given");
		retval = SLURM_SUCCESS;
		goto CLEANUP;
	}

	// check if more modules are tried to be allocated than allowed
	if (parsed_options[_option_lookup("wmod")].num_arguments > MAX_ALLOCATED_MODULES) {
		snprintf(
			my_errmsg, MAX_ERROR_LENGTH,
			"Requested to many wafer modules: %zu requested %d allowed",
			parsed_options[_option_lookup("wmod")].num_arguments, MAX_ALLOCATED_MODULES);
		retval = SLURM_ERROR;
		goto CLEANUP;
	}

	// check if at least one wafer module given
	if (parsed_options[_option_lookup("wmod")].num_arguments == 0) {
		snprintf(my_errmsg, MAX_ERROR_LENGTH, "No wafer module given!");
		retval = ESLURM_INVALID_LICENSES;
		goto CLEANUP;
	}

	// check if only wmod option was given
	for (counter = 1; counter < NUM_OPTIONS; counter++) {
		if (parsed_options[counter].num_arguments > 0) {
			only_wmod_option = false;
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
		int number = 0;
		if (_str2int(parsed_options[_option_lookup("skip_master_alloc")].arguments[0], &number) !=
			NMPM_PLUGIN_SUCCESS) {
			snprintf(
				my_errmsg, MAX_ERROR_LENGTH, "Invalid skip-master-alloc argument %s",
				parsed_options[_option_lookup("skip_master_alloc")].arguments[argcount]);
			retval = ESLURM_INVALID_LICENSES;
			goto CLEANUP;
		}
		info("skip_master: %d", number);
		if (number == 1)
			skip_master_alloc = true;
		else if (number == 0)
			skip_master_alloc = false;
		else {
			snprintf(
				my_errmsg, MAX_ERROR_LENGTH, "Invalid skip-master-alloc argument %s",
				parsed_options[_option_lookup("skip_master_alloc")].arguments[argcount]);
			retval = ESLURM_INVALID_LICENSES;
			goto CLEANUP;
		}
	}

	// analyze wmod argument
	for (argcount = 0; argcount < parsed_options[_option_lookup("wmod")].num_arguments;
		 argcount++) {
		size_t wafer_id;
		bool valid_wafer;
		// get wafer ID
		if (_str2int(
				parsed_options[_option_lookup("wmod")].arguments[argcount], (int*) &wafer_id) !=
			NMPM_PLUGIN_SUCCESS) {
			snprintf(
				my_errmsg, MAX_ERROR_LENGTH, "Invalid wmod argument %s",
				parsed_options[_option_lookup("wmod")].arguments[argcount]);
			retval = ESLURM_INVALID_LICENSES;
			goto CLEANUP;
		}
		// check if wafer in hwdb
		if (hwdb4c_has_wafer_entry(hwdb_handle, wafer_id, &valid_wafer) != HWDB4C_SUCCESS ||
			!valid_wafer) {
			snprintf(my_errmsg, MAX_ERROR_LENGTH, "Wafer %zu not in hardware database", wafer_id);
			retval = ESLURM_INVALID_LICENSES;
			goto CLEANUP;
		}

		// check if wafer id already given
		for (counter = 0; counter < num_allocated_modules; counter++) {
			if (allocated_modules[counter].wafer_id == wafer_id) {
				snprintf(
					my_errmsg, MAX_ERROR_LENGTH, "Duplicate wafer module argument given %zu",
					wafer_id);
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
		num_allocated_modules++;
	}

	if (num_allocated_modules > 1 && !only_wmod_option) {
		snprintf(
			my_errmsg, MAX_ERROR_LENGTH,
			"multiple wafer modules given as well as additional options");
		retval = SLURM_ERROR;
		goto CLEANUP;
	}

	// look at other options if only one wafer module was spezified and other resource arguments
	else if (!only_wmod_option) {
		for (argcount = 0;
			 argcount < parsed_options[_option_lookup("reticle_without_aout")].num_arguments;
			 argcount++) {
			size_t reticle_id;
			if (_str2int(
					parsed_options[_option_lookup("reticle_without_aout")].arguments[argcount],
					(int*) &reticle_id) != NMPM_PLUGIN_SUCCESS) {
				snprintf(
					my_errmsg, MAX_ERROR_LENGTH, "Invalid --reticle_without_aout argument %s",
					parsed_options[_option_lookup("reticle_without_aout")].arguments[argcount]);
				retval = ESLURM_INVALID_LICENSES;
				goto CLEANUP;
			}
			if (_add_reticle(reticle_id, -1, &allocated_modules[0]) != NMPM_PLUGIN_SUCCESS) {
				snprintf(
					my_errmsg, MAX_ERROR_LENGTH, "Adding reticle_without_aout %s failed: %s",
					parsed_options[_option_lookup("reticle_without_aout")].arguments[argcount],
					function_error_msg);
				retval = ESLURM_INVALID_LICENSES;
				goto CLEANUP;
			}
		}
		for (argcount = 0;
			 argcount < parsed_options[_option_lookup("fpga_without_aout")].num_arguments;
			 argcount++) {
			size_t fpga_id;
			if (_str2int(
					parsed_options[_option_lookup("fpga_without_aout")].arguments[argcount],
					(int*) &fpga_id) != NMPM_PLUGIN_SUCCESS) {
				snprintf(
					my_errmsg, MAX_ERROR_LENGTH, "Invalid --fpga_without_aout argument %s",
					parsed_options[_option_lookup("fpga_without_aout")].arguments[argcount]);
				retval = ESLURM_INVALID_LICENSES;
				goto CLEANUP;
			}
			if (_add_fpga(fpga_id, -1, &allocated_modules[0]) != NMPM_PLUGIN_SUCCESS) {
				snprintf(
					my_errmsg, MAX_ERROR_LENGTH, "Adding fpga_without_aout %s failed: %s",
					parsed_options[_option_lookup("fpga_without_aout")].arguments[argcount],
					function_error_msg);
				retval = ESLURM_INVALID_LICENSES;
				goto CLEANUP;
			}
		}
		for (argcount = 0;
			 argcount < parsed_options[_option_lookup("hicann_without_aout")].num_arguments;
			 argcount++) {
			size_t hicann_id;
			if (_str2int(
					parsed_options[_option_lookup("hicann_without_aout")].arguments[argcount],
					(int*) &hicann_id) != NMPM_PLUGIN_SUCCESS) {
				snprintf(
					my_errmsg, MAX_ERROR_LENGTH, "Invalid hicann_without_aout argument %s",
					parsed_options[_option_lookup("hicann_without_aout")].arguments[argcount]);
				retval = ESLURM_INVALID_LICENSES;
				goto CLEANUP;
			}
			if (_add_hicann(hicann_id, -1, &allocated_modules[0]) != NMPM_PLUGIN_SUCCESS) {
				snprintf(
					my_errmsg, MAX_ERROR_LENGTH, "Adding hicann_without_aout %s failed: %s",
					parsed_options[_option_lookup("hicann_without_aout")].arguments[argcount],
					function_error_msg);
				retval = ESLURM_INVALID_LICENSES;
				goto CLEANUP;
			}
		}
		for (argcount = 0;
			 argcount <
			 parsed_options[_option_lookup("reticle_of_hicann_without_aout")].num_arguments;
			 argcount++) {
			size_t hicann_id;
			if (_str2int(
					parsed_options[_option_lookup("reticle_of_hicann_without_aout")]
						.arguments[argcount],
					(int*) &hicann_id) != NMPM_PLUGIN_SUCCESS) {
				snprintf(
					my_errmsg, MAX_ERROR_LENGTH,
					"Invalid reticle_of_hicann_without_aout argument %s",
					parsed_options[_option_lookup("reticle_of_hicann_without_aout")]
						.arguments[argcount]);
				retval = ESLURM_INVALID_LICENSES;
				goto CLEANUP;
			}
			if (_add_fpga_of_hicann(hicann_id, -1, &allocated_modules[0]) != NMPM_PLUGIN_SUCCESS) {
				snprintf(
					my_errmsg, MAX_ERROR_LENGTH,
					"Adding Reticle of HICANN wihtou aout %s failed: %s",
					parsed_options[_option_lookup("reticle_of_hicann_without_aout")]
						.arguments[argcount],
					function_error_msg);
				retval = ESLURM_INVALID_LICENSES;
				goto CLEANUP;
			}
		}
		for (argcount = 0;
			 argcount < parsed_options[_option_lookup("reticle_with_aout")].num_arguments;
			 argcount++) {
			size_t reticle_id;
			int aout;
			if (_split_aout_arg(
					parsed_options[_option_lookup("reticle_with_aout")].arguments[argcount],
					&reticle_id, &aout) != NMPM_PLUGIN_SUCCESS) {
				snprintf(
					my_errmsg, MAX_ERROR_LENGTH, "Invalid --reticle_with_aout argument %s",
					parsed_options[_option_lookup("reticle_with_aout")].arguments[argcount]);
				retval = ESLURM_INVALID_LICENSES;
				goto CLEANUP;
			}
			if (_add_reticle(reticle_id, aout, &allocated_modules[0]) != NMPM_PLUGIN_SUCCESS) {
				snprintf(
					my_errmsg, MAX_ERROR_LENGTH, "Adding reticle-with-aout %s failed: %s",
					parsed_options[_option_lookup("reticle_with_aout")].arguments[argcount],
					function_error_msg);
				retval = ESLURM_INVALID_LICENSES;
				goto CLEANUP;
			}
		}
		for (argcount = 0;
			 argcount < parsed_options[_option_lookup("fpga_with_aout")].num_arguments;
			 argcount++) {
			size_t fpga_id;
			int aout;
			if (_split_aout_arg(
					parsed_options[_option_lookup("fpga_with_aout")].arguments[argcount], &fpga_id,
					&aout) != NMPM_PLUGIN_SUCCESS) {
				snprintf(
					my_errmsg, MAX_ERROR_LENGTH, "Invalid --fpga_with_aout argument %s",
					parsed_options[_option_lookup("fpga_with_aout")].arguments[argcount]);
				retval = ESLURM_INVALID_LICENSES;
				goto CLEANUP;
			}
			if (_add_fpga(fpga_id, aout, &allocated_modules[0]) != NMPM_PLUGIN_SUCCESS) {
				snprintf(
					my_errmsg, MAX_ERROR_LENGTH, "Adding fpga-with-aout %s failed: %s",
					parsed_options[_option_lookup("fpga_with_aout")].arguments[argcount],
					function_error_msg);
				retval = ESLURM_INVALID_LICENSES;
				goto CLEANUP;
			}
		}
		for (argcount = 0;
			 argcount < parsed_options[_option_lookup("hicann_with_aout")].num_arguments;
			 argcount++) {
			size_t hicann_id;
			int aout;
			if (_split_aout_arg(
					parsed_options[_option_lookup("hicann_with_aout")].arguments[argcount],
					&hicann_id, &aout) != NMPM_PLUGIN_SUCCESS) {
				snprintf(
					my_errmsg, MAX_ERROR_LENGTH, "Invalid --hicann_with_aout argument %s",
					parsed_options[_option_lookup("hicann_with_aout")].arguments[argcount]);
				retval = ESLURM_INVALID_LICENSES;
				goto CLEANUP;
			}
			if (_add_hicann(hicann_id, aout, &allocated_modules[0]) != NMPM_PLUGIN_SUCCESS) {
				snprintf(
					my_errmsg, MAX_ERROR_LENGTH, "Adding hicann-with-aout %s failed: %s",
					parsed_options[_option_lookup("hicann_with_aout")].arguments[argcount],
					function_error_msg);
				retval = ESLURM_INVALID_LICENSES;
				goto CLEANUP;
			}
		}
		for (argcount = 0;
			 argcount < parsed_options[_option_lookup("reticle_of_hicann_with_aout")].num_arguments;
			 argcount++) {
			size_t hicann_id;
			int aout;
			if (_split_aout_arg(
					parsed_options[_option_lookup("reticle_of_hicann_with_aout")]
						.arguments[argcount],
					&hicann_id, &aout) != NMPM_PLUGIN_SUCCESS) {
				snprintf(
					my_errmsg, MAX_ERROR_LENGTH,
					"Invalid --reticle_of_hicann_with_aout argument %s",
					parsed_options[_option_lookup("reticle_of_hicann_with_aout")]
						.arguments[argcount]);
				retval = ESLURM_INVALID_LICENSES;
				goto CLEANUP;
			}
			if (_add_fpga_of_hicann(hicann_id, aout, &allocated_modules[0]) !=
				NMPM_PLUGIN_SUCCESS) {
				snprintf(
					my_errmsg, MAX_ERROR_LENGTH, "Adding reticle-of-hicann-with-aout %s failed: %s",
					parsed_options[_option_lookup("reticle_of_hicann_with_aout")]
						.arguments[argcount],
					function_error_msg);
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
				if (hwdb4c_has_fpga_entry(
						hwdb_handle,
						allocated_modules[modulecounter].wafer_id * NUM_FPGAS_ON_WAFER +
							fpgacounter,
						&has_fpga_entry) != HWDB4C_SUCCESS) {
					snprintf(
						my_errmsg, MAX_ERROR_LENGTH,
						"Adding whole Module %zu: FPGAOnWafer out of range %zu, this should never "
						"happen?!?",
						allocated_modules[modulecounter].wafer_id, fpgacounter);
					retval = ESLURM_INVALID_LICENSES;
					goto CLEANUP;
				}
				if (has_fpga_entry) {
					// check for both possible adcs
					bool has_adc0_entry, has_adc1_entry;
					if (hwdb4c_has_adc_entry(
							hwdb_handle,
							allocated_modules[modulecounter].wafer_id * NUM_FPGAS_ON_WAFER +
								fpgacounter,
							0, &has_adc0_entry) != HWDB4C_SUCCESS) {
						snprintf(
							my_errmsg, MAX_ERROR_LENGTH,
							"FPGA %zu on Wafer-Module %zu has no ADC for AnalogOnHICANN 0",
							fpgacounter, allocated_modules[modulecounter].wafer_id);
						retval = ESLURM_INVALID_LICENSES;
						goto CLEANUP;
					}
					if (hwdb4c_has_adc_entry(
							hwdb_handle,
							allocated_modules[modulecounter].wafer_id * NUM_FPGAS_ON_WAFER +
								fpgacounter,
							1, &has_adc1_entry) != HWDB4C_SUCCESS) {
						snprintf(
							my_errmsg, MAX_ERROR_LENGTH,
							"FPGA %zu on Wafer-Module %zu has no ADC for AnalogOnHICANN 1",
							fpgacounter, allocated_modules[modulecounter].wafer_id);
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
					if (_add_fpga(fpgacounter, aout, &allocated_modules[modulecounter]) !=
						NMPM_PLUGIN_SUCCESS) {
						snprintf(
							my_errmsg, MAX_ERROR_LENGTH,
							"Adding whole Module %zu: Adding FPGA %zu failed: %s",
							allocated_modules[modulecounter].wafer_id, fpgacounter,
							function_error_msg);
						retval = ESLURM_INVALID_LICENSES;
						goto CLEANUP;
					}
				}
			}
		}
	}

	// generate slurm license string from allocated modules
	slurm_licenses_string =
		malloc(sizeof(char) * num_allocated_modules * MAX_LICENSE_STRING_LENGTH_PER_WAFER);
	if (slurm_licenses_string == NULL) {
		snprintf(my_errmsg, MAX_ERROR_LENGTH, "Memory alloc for slurm license string failed");
		retval = SLURM_ERROR;
		goto CLEANUP;
	}
	strcpy(slurm_licenses_string, "");
	hicann_environment_string =
		malloc(sizeof(char) * num_allocated_modules * MAX_HICANN_ENV_LENGTH_PER_WAFER);
	if (hicann_environment_string == NULL) {
		snprintf(my_errmsg, MAX_ERROR_LENGTH, "Memory alloc for HICANN environment string failed");
		retval = SLURM_ERROR;
		goto CLEANUP;
	}
	strcpy(hicann_environment_string, ALLOCATED_HICANN_ENV_NAME);
	adc_environment_string =
		malloc(sizeof(char) * num_allocated_modules * MAX_ADC_ENV_LENGTH_PER_WAFER);
	if (adc_environment_string == NULL) {
		snprintf(my_errmsg, MAX_ERROR_LENGTH, "Memory alloc for ADC envrionment string failed");
		retval = SLURM_ERROR;
		goto CLEANUP;
	}
	strcpy(adc_environment_string, ALLOCATED_ADC_ENV_NAME);

	for (modulecounter = 0; modulecounter < num_allocated_modules; modulecounter++) {
		char tempstring[MAX_ADC_COORD_LENGTH];
		size_t fpgacounter = 0;
		size_t num_active_fpgas = 0;
		size_t adccounter = 0;
		size_t hicanncounter = 0;
		// check if more than one FPGA was requested, if true also request master FPGA
		if (!skip_master_alloc) {
			for (fpgacounter = 0; fpgacounter < NUM_FPGAS_ON_WAFER; fpgacounter++) {
				if (allocated_modules[modulecounter].active_fpgas[fpgacounter]) {
					num_active_fpgas++;
				}
				if (num_active_fpgas > 1) {
					allocated_modules[modulecounter].active_fpgas[hwdb4c_master_FPGA_enum()] = true;
					break;
				}
			}
		}
		for (fpgacounter = 0; fpgacounter < NUM_FPGAS_ON_WAFER; fpgacounter++) {
			if (allocated_modules[modulecounter].active_fpgas[fpgacounter]) {
				snprintf(
					tempstring, MAX_ADC_COORD_LENGTH, "W%zuF%zu,",
					allocated_modules[modulecounter].wafer_id, fpgacounter);
				strcat(slurm_licenses_string, tempstring);
			}
		}
		for (hicanncounter = 0; hicanncounter < NUM_HICANNS_ON_WAFER; hicanncounter++) {
			if (allocated_modules[modulecounter].active_hicanns[hicanncounter]) {
				snprintf(
					tempstring, MAX_ADC_COORD_LENGTH, "%zu,",
					allocated_modules[modulecounter].wafer_id * NUM_HICANNS_ON_WAFER +
						hicanncounter);
				strcat(hicann_environment_string, tempstring);
			}
		}
		for (adccounter = 0; adccounter < allocated_modules[modulecounter].num_active_adcs;
			 adccounter++) {
			snprintf(
				tempstring, MAX_ADC_COORD_LENGTH, "%s,",
				allocated_modules[modulecounter].active_adcs[adccounter]);
			strcat(slurm_licenses_string, tempstring);
			strcat(adc_environment_string, tempstring);
		}
	}

	// delete trailing ','
	if (strlen(slurm_licenses_string) > 1)
		slurm_licenses_string[strlen(slurm_licenses_string) - 1] = '\0';
	if (strlen(hicann_environment_string) > 1)
		hicann_environment_string[strlen(hicann_environment_string) - 1] = '\0';
	if (strlen(adc_environment_string) > 1)
		adc_environment_string[strlen(adc_environment_string) - 1] = '\0';

	// FIXME: find a way to implement this for srun
	if (job_desc->environment) {
		xrealloc(job_desc->environment, sizeof(char*) * (job_desc->env_size + 2));
		job_desc->environment[job_desc->env_size] = xstrdup(hicann_environment_string);
		job_desc->environment[job_desc->env_size + 1] = xstrdup(adc_environment_string);
		job_desc->env_size = job_desc->env_size + 2;
	}

	// set slurm licenses
	xrealloc(job_desc->licenses, sizeof(char*) * (strlen(slurm_licenses_string)));
	job_desc->licenses = xstrdup(slurm_licenses_string);
	info("LICENSES: %s", job_desc->licenses);
	retval = SLURM_SUCCESS;

CLEANUP:

	if (retval != SLURM_SUCCESS) {
		*err_msg = xstrdup(my_errmsg);
		error("%s", my_errmsg);
	}
	if (slurm_licenses_string)
		free(slurm_licenses_string);
	if (hicann_environment_string)
		free(hicann_environment_string);
	if (adc_environment_string)
		free(adc_environment_string);
	if (hwdb_handle) {
		hwdb4c_free_hwdb(hwdb_handle);
		hwdb_handle = NULL;
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
		return NMPM_PLUGIN_FAILURE;
	errno = 0;
	value = strtol(str, &end, 10);
	if (end == str || *end != '\0' || (errno == ERANGE && (value == LONG_MAX || value == LONG_MIN)))
		return NMPM_PLUGIN_FAILURE;
	if (value > INT_MAX || value < INT_MIN)
		return NMPM_PLUGIN_FAILURE;
	*p2int = (int) value;
	return NMPM_PLUGIN_SUCCESS;
}

static int _split_aout_arg(char const* arg, size_t* value, int* aout)
{
	char* aout_split;
	if (strstr(arg, ":") == NULL) {
		if (_str2int(arg, (int*) value) != NMPM_PLUGIN_SUCCESS)
			return NMPM_PLUGIN_FAILURE;
		*aout = 2;
	} else {
		aout_split = strtok(arg, ":");
		if (_str2int(aout_split, (int*) value) != NMPM_PLUGIN_SUCCESS)
			return NMPM_PLUGIN_FAILURE;
		aout_split = strtok(NULL, ",");
		if (_str2int(aout_split, aout) != NMPM_PLUGIN_SUCCESS)
			return NMPM_PLUGIN_FAILURE;
	}
	return NMPM_PLUGIN_SUCCESS;
}

static int _option_lookup(char const* option_string)
{
	size_t indexcounter;
	for (indexcounter = 0; indexcounter < NUM_OPTIONS; indexcounter++) {
		if (strcmp(custom_res_options[indexcounter].option_name, option_string) == 0) {
			return custom_res_options[indexcounter].index;
		}
	}
	return NMPM_PLUGIN_FAILURE;
}

static int _parse_options(
	struct job_descriptor const* job_desc, struct option_entry* parsed_options, bool* zero_res_args)
{
	size_t optioncount, argcount;
	char argumentsrc[MAX_ARGUMENT_CHAIN_LENGTH];
	char* arguments;
	char* option;
	char* argument_token;
	// each option is formated the following way
	// _SLURM_SPANK_OPTION_wafer_res_opts_[option]=[argument,argument,...]
	// we iterate over all arguments of all options and save them in parsed_options
	for (optioncount = 0; optioncount < job_desc->spank_job_env_size; optioncount++) {
		char* spank_option_str = job_desc->spank_job_env[optioncount];
		option = strstr(spank_option_str, SPANK_OPT_PREFIX);
		if (option == NULL) {
			// some other spank option, skip
			continue;
		}
		option += strlen(SPANK_OPT_PREFIX); // truncate SPANK_OPT_PREFIX
		strncpy(argumentsrc, option, MAX_ARGUMENT_CHAIN_LENGTH);
		arguments = strstr(argumentsrc, "="); // get string after = symbol
		if (arguments == NULL) {
			snprintf(
				function_error_msg, MAX_ERROR_LENGTH,
				"'=' not present in spank option string, this should never happen");
			return NMPM_PLUGIN_FAILURE;
		}
		option[strlen(option) - strlen(arguments)] = 0; // truncate '=' at end of option string
		arguments += 1; // truncate '=' at beginning of argument chain
		if (strlen(arguments) > MAX_ARGUMENT_CHAIN_LENGTH) {
			snprintf(
				function_error_msg, MAX_ERROR_LENGTH, "To long argument, over %d chars",
				MAX_ARGUMENT_CHAIN_LENGTH);
			return NMPM_PLUGIN_FAILURE;
		}

		argument_token = strtok(arguments, ",");
		argcount = 0;
		if (_option_lookup(option) < 0) {
			snprintf(
				function_error_msg, MAX_ERROR_LENGTH,
				"Invalid option %s, please update spank arguments", option);
			return NMPM_PLUGIN_FAILURE;
		}
		*zero_res_args = false;
		while (argument_token != NULL) {
			strcpy(parsed_options[_option_lookup(option)].arguments[argcount], argument_token);
			argcount++;
			parsed_options[_option_lookup(option)].num_arguments = argcount;
			argument_token = strtok(NULL, ",");
		}
	}
	return NMPM_PLUGIN_SUCCESS;
}

static int _add_reticle(size_t reticle_id, int aout, struct wafer_res_t* allocated_module)
{
	size_t fpga_id;
	if (hwdb4c_ReticleOnWafer_toFPGAOnWafer(reticle_id, &fpga_id) != HWDB4C_SUCCESS) {
		return NMPM_PLUGIN_FAILURE;
	}
	return _add_fpga(fpga_id, aout, allocated_module);
}

static int _add_fpga_of_hicann(size_t hicann_id, int aout, struct wafer_res_t* allocated_module)
{
	size_t fpga_id;
	if (hwdb4c_HICANNOnWafer_toFPGAOnWafer(hicann_id, &fpga_id) != HWDB4C_SUCCESS) {
		return NMPM_PLUGIN_FAILURE;
	}
	return _add_fpga(fpga_id, aout, allocated_module);
}

static int _add_fpga(size_t fpga_id, int aout, struct wafer_res_t* allocated_module)
{
	size_t hicanncounter;
	struct hwdb4c_hicann_entry** hicann_entries;
	size_t num_hicanns;
	bool has_fpga_entry;
	size_t global_fpga_id = allocated_module->wafer_id * NUM_FPGAS_ON_WAFER + fpga_id;

	// check if fpga is in hwdb_handle
	if (hwdb4c_has_fpga_entry(hwdb_handle, global_fpga_id, &has_fpga_entry) != HWDB4C_SUCCESS ||
		!has_fpga_entry) {
		snprintf(
			function_error_msg, MAX_ERROR_LENGTH, "FPGA %zu on Wafer-Module %zu not in HWDB",
			fpga_id, allocated_module->wafer_id);
		return NMPM_PLUGIN_FAILURE;
	}

	if (hwdb4c_get_hicann_entries_of_FPGAGlobal(
			hwdb_handle, global_fpga_id, &hicann_entries, &num_hicanns) != HWDB4C_SUCCESS) {
		snprintf(
			function_error_msg, MAX_ERROR_LENGTH,
			"Failed to get HICANN entries for FPGA %zu on Wafer-Module %zu ", fpga_id,
			allocated_module->wafer_id);
		return NMPM_PLUGIN_FAILURE;
	}

	// add_hicanns
	for (hicanncounter = 0; hicanncounter < num_hicanns; hicanncounter++) {
		allocated_module->active_hicanns
			[hicann_entries[hicanncounter]->hicannglobal_id % NUM_HICANNS_ON_WAFER] = true;
	}

	hwdb4c_free_hicann_entries(hicann_entries, num_hicanns);

	allocated_module->active_fpgas[fpga_id] = true;
	if (aout > -1)
		if (_add_adc(fpga_id, aout, allocated_module) != NMPM_PLUGIN_SUCCESS)
			return NMPM_PLUGIN_FAILURE;
	return NMPM_PLUGIN_SUCCESS;
}

static int _add_hicann(size_t hicann_id, int aout, struct wafer_res_t* allocated_module)
{
	bool has_hicann_entry;
	bool has_fpga_entry;
	size_t fpga_id;

	// check if HICANN is in hwdb
	if (hwdb4c_has_hicann_entry(
			hwdb_handle, allocated_module->wafer_id * NUM_HICANNS_ON_WAFER + hicann_id,
			&has_hicann_entry) != HWDB4C_SUCCESS ||
		!has_hicann_entry) {
		snprintf(
			function_error_msg, MAX_ERROR_LENGTH, "HICANN %zu on Wafer-Module %zu not in HWDB",
			hicann_id, allocated_module->wafer_id);
		return NMPM_PLUGIN_FAILURE;
	}
	if (hwdb4c_HICANNOnWafer_toFPGAOnWafer(hicann_id, &fpga_id) != HWDB4C_SUCCESS) {
		snprintf(
			function_error_msg, MAX_ERROR_LENGTH,
			"Failed to convert HICANN %zu on Wafer-Module %zu to FPGA", hicann_id,
			allocated_module->wafer_id);
		return NMPM_PLUGIN_FAILURE;
	}
	// check if FPGA is in hwdb
	if (hwdb4c_has_fpga_entry(
			hwdb_handle, allocated_module->wafer_id * NUM_FPGAS_ON_WAFER + fpga_id,
			&has_fpga_entry) != HWDB4C_SUCCESS ||
		!has_fpga_entry) {
		snprintf(
			function_error_msg, MAX_ERROR_LENGTH,
			"FPGA %zu for HICANN %zu on Wafer-Module %zu not in HWDB", fpga_id, hicann_id,
			allocated_module->wafer_id);
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

static int _add_adc(size_t fpga_id, int aout, struct wafer_res_t* allocated_module)
{
	char adc_license[MAX_ADC_COORD_LENGTH];
	size_t adccounter;
	size_t aoutcounter;
	size_t aoutbegin;
	size_t aoutend;
	struct hwdb4c_adc_entry* adc_entry;
	size_t global_fpga_id = allocated_module->wafer_id * NUM_FPGAS_ON_WAFER + fpga_id;
	bool has_adc_entry;
	if (aout == 0 || aout == 1) {
		aoutbegin = aout;
		aoutend = aout + 1;
	} else if (aout == 2) {
		aoutbegin = 0;
		aoutend = 2;
	} else {
		snprintf(function_error_msg, MAX_ERROR_LENGTH, "AnalogOnHICANN %d out of range", aout);
		return NMPM_PLUGIN_FAILURE;
	}
	for (aoutcounter = aoutbegin; aoutcounter < aoutend; aoutcounter++) {
		if (hwdb4c_has_adc_entry(hwdb_handle, global_fpga_id, aoutcounter, &has_adc_entry) !=
				HWDB4C_SUCCESS &&
			!has_adc_entry) {
			snprintf(
				function_error_msg, MAX_ERROR_LENGTH,
				"ADC Entry (FPGAGlobal %zu, AnalogOnHICANN %zu) not in HWDB", global_fpga_id,
				aoutcounter);
			return NMPM_PLUGIN_FAILURE;
		}
		if (hwdb4c_get_adc_entry(hwdb_handle, global_fpga_id, aoutcounter, &adc_entry) !=
			HWDB4C_SUCCESS) {
			snprintf(
				function_error_msg, MAX_ERROR_LENGTH,
				"get ADC Entry (FPGAGlobal %zu, AnalogOnHICANN %zu) failed", global_fpga_id,
				aoutcounter);
			return NMPM_PLUGIN_FAILURE;
		}
		strncpy(adc_license, adc_entry->coord, MAX_ADC_COORD_LENGTH);
		// check if license is already requested
		for (adccounter = 0; adccounter < allocated_module->num_active_adcs; adccounter++) {
			if (strcmp(adc_license, allocated_module->active_adcs[adccounter]) == 0)
				// license already in list of to be requested licenses
				return NMPM_PLUGIN_SUCCESS;
		}
		// check if requesting to many adcs
		hwdb4c_free_adc_entry(adc_entry);
		if (allocated_module->num_active_adcs + 1 > MAX_ADCS_PER_WAFER) {
			snprintf(
				function_error_msg, MAX_ERROR_LENGTH,
				"Requesting more ADC licenses than available on one module (Wmod %zu)",
				allocated_module->wafer_id);
			return NMPM_PLUGIN_FAILURE;
		}
		strcpy(allocated_module->active_adcs[allocated_module->num_active_adcs], adc_license);
		allocated_module->num_active_adcs++;
	}
	return NMPM_PLUGIN_SUCCESS;
}

// vim: sw=4 ts=4 sts=4 noexpandtab
