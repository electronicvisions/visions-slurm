#pragma once

// This file is to be included in hagen_daas_config.c
// and only contains the default config parameters

// runtime constants (can be overwritten via hagen_daas.toml)
hd_config_t const hagen_daas_defaults = {
	// scoop defines
	.env_content_magic = "ReubenRose",
	.env_name_magic = "USES_HAGEN_DAAS",
	.env_name_scoop_board_id = "QUIGGELDY_BOARD",
	.env_name_scoop_ip = "QUIGGELDY_IP",
	.env_name_scoop_job_id = "SCOOP_JOB_ID",
	.env_name_scoop_port = "QUIGGELDY_PORT",
	.env_name_error_msg = "HAGEN_DAAS_ERROR_MSG",

	.services = NULL,
	.num_services = 0,

	// hagen daas defines
	// first port used by scoops
	.scoop_port_lowest = 12321,

	// jobname fmt specifier having one string placeholder for the board_id
	.scoop_jobname_prefix = "scoop_",

	.scoop_job_user = "slurm",

	// working directory into which slurm logs etc are being placed
	.scoop_working_dir = "/tmp",

	// how many seconds does a compute job wait once the scoop has been started?
	.scoop_launch_wait_secs = 3,

	// how many seconds is a started scoop job still considered pending
	.scoop_pending_secs = 5,

	// time to wait till checking again if scoop is running in srun calls
	.srun_requeue_wait_period_secs = 10,
	.srun_requeue_wait_num_periods = 10,

	// time to wait for scoop launch job to appear in queue
	.scoop_launch_wait_period_secs = 1,
	.scoop_launch_wait_num_periods = 10
};

// vim: sw=4 ts=4 sts=4 noexpandtab tw=80
