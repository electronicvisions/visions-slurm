#pragma once

/*********************************************
 * common compile-time defines and constants *
 *********************************************/

#define CERBERUS_PLUGIN_SUCCESS 0
#define CERBERUS_PLUGIN_FAILURE -1

/**************************
 * common data structures *
 *************************/

typedef struct watched_partition
{
	char* name;
	uint32_t num_allowed_jobs_per_user;
} watched_partition_t;

// all configuration in one file
typedef struct crb_config
{
	watched_partition_t* partitions;
	uint32_t num_partitions;
} crb_config_t;

// initialize crb_config_t data structure
void crb_config_t_init(crb_config_t**);

// Free crb_config_t data structure
// and sets it to NULL
void crb_config_t_free(crb_config_t**);

// Load crb_config_t from file
// -> caller has to call crb_config_t_free on the given variable!
int crb_config_t_load(crb_config_t**);
