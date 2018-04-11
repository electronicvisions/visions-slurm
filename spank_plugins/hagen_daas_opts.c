/*
 *   Howto Avoid Grabbing Emulators Nightlong: DLS as a Service
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
SPANK_PLUGIN(hagen_daas_opts, 1);

/*
 * #define MAX_JOBNAME_LEN 
 * static char _jobname_prefix[MAX_JOBNAME_LEN];
 */

static int _check_opt(int val, const char* optarg, int remote);

struct spank_option my_spank_options[] = {
	{	"daas-board-id", "[board-id]",
		"Board id (currently USB serial, same as gres) of the hardware board to connect to.",
		1, 0, (spank_opt_cb_f) _check_opt},
	{	"dbid", "[board-id]",
		"Shortcut for --daas-board-id.",
		1, 0, (spank_opt_cb_f) _check_opt},
	{	"start-scoop", "[board-id]",
		"Start a scoop (arbiter daemon) for the given board id.",
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

	return 0;
}

// vim: sw=4 ts=4 sts=4 noexpandtab tw=120
