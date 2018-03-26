// Common defines used in several plugins

/********************************
 * common defines and constants *
 *******************************/

char const hd_spank_prefix[] = "_SLURM_SPANK_OPTION_hagen_daas_";

// scoop defines
char const hd_env_content_magic[] = "ReubenRose";
char const hd_env_name_magic[] = "USES_HAGEN_DAAS";
char const hd_env_name_scoop_board_id[] = "QUIGGELDY_BOARD";
char const hd_env_name_scoop_ip[] = "QUIGGELDY_IP";
char const hd_env_name_scoop_job_id[] = "SCOOP_JOB_ID";
char const hd_env_name_scoop_port[] = "QUIGGELDY_PORT";
char const hd_env_name_error_msg[] = "HAGEN_DAAS_ERROR_MSG";

// both define and macro because the latter is not compile time constant
#define HAGEN_DAAS_OPT_NAME_LAUNCH_SCOOP "launch_scoop"
char const hd_opt_name_launch_scoop[] = HAGEN_DAAS_OPT_NAME_LAUNCH_SCOOP;

// hagen daas defines
// jobname fmt specifier having one string placeholder for the board_id
char const hd_scoop_jobname_prefix[] = "quiggeldy_";

char const hd_scoop_job_user[] = "slurm";

// working directory into which slurm logs etc are being placed
char const hd_scoop_working_dir[] = "/tmp";

// how many seconds does a compute job wait once the scoop has been started?
int const hd_scoop_launch_wait_secs = 3;

// how many seconds is a started scoop job still considered pending
int const hd_scoop_pending_secs = 5;

// time to wait till checking again if scoop is running in srun calls
int const hd_srun_requeue_wait_period_secs = 10;
int const hd_srun_requeue_wait_num_periods = 10;

// time to wait for scoop launch job to appear in queue
int const hd_scoop_launch_wait_period_secs = 1;
int const hd_scoop_launch_wait_num_periods = 10;

#define HAGEN_DAAS_PLUGIN_SUCCESS 0
#define HAGEN_DAAS_PLUGIN_FAILURE -1
#define HAGEN_DAAS_PLUGIN_NO_JOB_NEEDED 1

#define MAX_NUM_ARGUMENTS 64
#define MAX_LENGTH_ARGUMENT_CHAIN 16384 // max number of chars for one argument
#define MAX_LENGTH_ARGUMENT 64          // max number of chars for one element of an argument chain
#define MAX_LENGTH_ERROR 8192
#define MAX_LENGTH_OPTION_NAME 64
#define MAX_LENGTH_SERVICE_NAME 64

/**************************
 * common data structures *
 *************************/

typedef struct service
{
	char service_name[MAX_LENGTH_SERVICE_NAME];
	char script_path[PATH_MAX];
	char slurm_account[64];
	char slurm_partition[64];
	uint16_t port;
	uint16_t cpus; // how many cpus does the scoop job need
	uint64_t memory_in_mb;
} service_t;

// TODO move this to configuration file
// setups used for NICE demo:
// 07
// B201330
// B291698
// B291656
#define NUM_SERVICES 4
static const service_t service_infos[NUM_SERVICES] = {
	{	"dls-v2-for-07",						// service_name
		"/etc/slurm-config/run_quiggeldy.sh",	// script_path
		"hagen_daas", "dls",					// slurm_account / slurm_partition
		5666,									// port
		1,										// cpus
		1536									// memory_in_mb
	},
	{	"dls-v2-for-B201330",					// service_name
		"/etc/slurm-config/run_quiggeldy.sh",	// script_path
		"hagen_daas", "dls",					// slurm_account / slurm_partition
		5667,									// port
		1,										// cpus
		1536									// memory_in_mb
	},
	{	"dls-v2-for-B291698",					// service_name
		"/etc/slurm-config/run_quiggeldy.sh",	// script_path
		"hagen_daas", "dls",					// slurm_account / slurm_partition
		5668,									// port
		1,										// cpus
		1536									// memory_in_mb
	},
	{	"dls-v2-for-B291656",					// service_name
		"/etc/slurm-config/run_quiggeldy.sh",	// script_path
		"hagen_daas", "dls",					// slurm_account / slurm_partition
		5669,									// port
		1,										// cpus
		1536									// memory_in_mb
	},
};

// vim: sw=4 ts=4 sts=4 noexpandtab tw=80
