/*
 *   Hardware Multiplexing As A Service (hmaas)
 *
 *   To compile:
 *    gcc -shared -o hmaas_opts.so hmaas_opts.c
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
 *
 * Supported spank options are job-id prefix;
 */
SPANK_PLUGIN(hmaas, 1);

/*
 * #define MAX_JOBNAME_LEN 
 * static char _jobname_prefix[MAX_JOBNAME_LEN];
 */

static int _check_opt(int val, const char* optarg, int remote);

struct spank_option my_spank_options[] = {
	{"hmaas-board-id", "[board-id]",
	"Board id (currently USB serial, same as gres) of the hardware board to connect to.",
	1, 0, (spank_opt_cb_f) _check_opt},
	{"hbid", "[board-id]",
	"Shortcut for --hmaas-board-id.",
	1, 0, (spank_opt_cb_f) _check_opt},
	SPANK_OPTIONS_TABLE_END};

static int _check_opt(int val, const char* optarg, int remote)
{
	if (optarg == NULL)
		return -1;
	return 0;
}

int slurm_spank_init(spank_t sp, int ac, char** av)
{
	size_t optioncounter;
	struct spank_option endmarker = SPANK_OPTIONS_TABLE_END;
	for (optioncounter = 0; my_spank_options[optioncounter].name != endmarker.name;
		 optioncounter++) {
		if (spank_option_register(sp, &my_spank_options[optioncounter]) != ESPANK_SUCCESS)
			return -1;
	}

	/*
	 * // process plugin options
	 * for (i = 0; i < ac; i++) {
	 *     if (strncmp ("jobname_prefix=", av[i], 15) == 0) {
	 *         const char *optarg = av[i] + 15;
	 *         strncpy (&_jobname_prefix, optarg, MAX_JOBNAME_LEN);
	 *     } else {
	 *         slurm_error ("hmaas: Invalid option: %s", av[i]);
	 *     }
	 * }
	 */

	return 0;
}

// vim: sw=4 ts=4 sts=4 noexpandtab
