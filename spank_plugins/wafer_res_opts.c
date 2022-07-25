/*
 *  This plugin provides paramters to srun/sbatch which are used in the
 *  nmpm custom resource job submit plugin.
 *
 *  This plugin has been developed by staff and students
 *  of Heidelberg University as part of the research carried out by the
 *  Electronic Vision(s) group at the Kirchhoff-Institute for Physics.
 *  The research is funded by Heidelberg University, the State of
 *  Baden-Württemberg, the Seventh Framework Programme under grant agreements
 *  no 604102 (HBP) as well as the Horizon 2020 Framework Programme under grant 
 *  agreement 720270 (HBP).
 *
 *   To compile:
 *    gcc -shared -o wafer_res_opts.so wafer_res_opts.c
 *
 */
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/resource.h>
#include <sys/types.h>

#include <slurm/spank.h>

/*
 * All spank plugins must define this macro for the
 * Slurm plugin loader.
 */
SPANK_PLUGIN(wafer_res_opts, 1);


static int _check_opt(int val, const char* optarg, int remote);
static int _no_op(int val, const char* optarg, int remote);

struct spank_option my_spank_options[] = {
    {"wmod", "[modulenum],[...],...",
     "Comma seperated list of wafer modules. If only wmod option "
     "given all resources of module(s) are allocated. Other "
     "options only apply if exactly one module was specified.",
     1, 0, (spank_opt_cb_f)_check_opt},
    {"wafer", "[modulenum],[...],...", "Same as --wmod.", 1, 0, (spank_opt_cb_f)_check_opt},
    {"fpga-with-aout", "[fpganum[:0/1]],[fpganum],...",
     "Comma seperated list of FPGAs whose corresponding two ADCs should also be allocated. "
     "Optionally can specify which of the two ADCs should be allocated with 0/1 sperated by colon.",
     1, 0, (spank_opt_cb_f)_check_opt},
    {"reticle-with-aout", "[reticlenum:0/1],[reticlenum],...",
     "Comma seperated list of Reticles whose corresponding two ADCs should also be allocated. "
     "Optionally can specify which of the two ADCs should be allocated with 0/1 sperated by colon.",
     1, 0, (spank_opt_cb_f)_check_opt},
    {"hicann-with-aout", "[hicannnum:0/1],[hicannnum],...",
     "Comma seperated list of HICANNs whose corresponding two ADCs should also be allocated. "
     "Optionally can specify which of the two ADCs should be allocated with 0/1 sperated by colon.",
     1, 0, (spank_opt_cb_f)_check_opt},
    {"reticle-of-hicann-with-aout", "[hicannnum:0/1],[hicannnum],..",
     "Comma seperated list of HICANNs whose corresponding Reticle and two ADCs should also be "
     "allocated. Optionally can specify which of the two ADCs should be allocated with 0/1 "
     "sperated by colon.",
     1, 0, (spank_opt_cb_f)_check_opt},
    {"fpga", "[fpganum[:0/1]],[fpganum],...",
     "Same as --fpga-with-aout.",
     1, 0, (spank_opt_cb_f)_check_opt},
    {"reticle", "[reticlenum:0/1],[reticlenum],...",
     "Same as --reticle-with-aout.",
     1, 0, (spank_opt_cb_f)_check_opt},
    {"hicann", "[hicannnum:0/1],[hicannnum],...",
     "Same as --hicann-with-aout",
     1, 0, (spank_opt_cb_f)_check_opt},
    {"reticle-of-hicann", "[hicannnum:0/1],[hicannnum],..",
     "Same as reticle-of-hicann-with-aout",
     1, 0, (spank_opt_cb_f)_check_opt},
    {"fpga-without-aout", "[fpganum],[...],...",
     "Comma seperated list of FPGAs.",
     1, 0, (spank_opt_cb_f)_check_opt},
    {"reticle-without-aout", "[reticlenum],[...],...",
     "Comma seperated list of Reticles.",
     1, 0, (spank_opt_cb_f)_check_opt},
    {"hicann-without-aout", "[hicannnum],[...],...",
     "Comma seperated list of HICANNs.",
     1, 0, (spank_opt_cb_f)_check_opt},
    {"reticle-of-hicann-without-aout", "[hicannnum],[...],...",
     "Comma seperated list of HICANNs whose Reticles should be allocated.",
     1, 0, (spank_opt_cb_f)_check_opt},
    {"hwdb-path", "[path/to/custom/hwdb]",
     "Optional path to custom hardware database. If not given default hardware "
     "database path is used.",
     1, 0, (spank_opt_cb_f)_check_opt},
    {"skip-master-alloc", "",
     "Skip the automated allocation of the master FPGA (12) in case multiple "
     "FPGAs are requested",
     0, 0, (spank_opt_cb_f)_no_op},
    {"without-trigger", "",
     "Skip the automated allocation of adc trigger group licenses",
     0, 0, (spank_opt_cb_f)_no_op},
    {"allocate-aggregator", "0/1",
     "Allocate the aggregator board for HX multi chip systems (wafer ID >80). "
     "Defaults to 1 (true)",
     1, 0, (spank_opt_cb_f)_check_opt},
    {"skip-hicann-init", "",
     "Skip the automated initialization of neighbouring licenses"
     "Cannot be specified together with 'force-hicann-init'.",
     0, 0, (spank_opt_cb_f)_no_op},
    {"force-hicann-init", "",
     "Force the automated initialization of all neighbouring licenses"
     "Cannot be specified together with 'skip-hicann-init'.",
     0, 0, (spank_opt_cb_f)_no_op},
    {"defects-path", "[path/to/custom/blacklisting]",
     "Path to directory containing blacklisting information.",
     1, 0, (spank_opt_cb_f)_check_opt},
    SPANK_OPTIONS_TABLE_END};

static int _check_opt(int val, const char* optarg, int remote)
{
	if ((optarg == NULL) || (strcmp(optarg, "") == 0)) {
		slurm_error("Empty argument provided");
		return ESPANK_BAD_ARG;
	}
	return ESPANK_SUCCESS;
}

static int _no_op(int val, const char* optarg, int remote)
{
	return ESPANK_SUCCESS;
}

int slurm_spank_init(spank_t sp, int ac, char** av)
{
	size_t optioncounter;
	struct spank_option endmarker = SPANK_OPTIONS_TABLE_END;
	for (optioncounter = 0; my_spank_options[optioncounter].name != endmarker.name;
	     optioncounter++) {
		if (spank_option_register(sp, &my_spank_options[optioncounter]) != ESPANK_SUCCESS)
			return ESPANK_ERROR;
	}
	return ESPANK_SUCCESS;
}
