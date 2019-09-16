#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h> 
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "src/common/read_config.h"
#include "src/common/xmalloc.h"
#include "src/common/xstring.h"

#include "toml.h"

#include "hagen_daas_config.h"
#include "hagen_daas_config_default.h"

hd_config_t* hagen_daas_config = NULL;

// extract relevant configuration data from already parsed toml tree
static int _toml_table_to_hd_config(toml_table_t* root, hd_config_t* cfg);

// read a single service from a node (toml_node_walker)
static int _toml_read_services(toml_array_t*, hd_config_t* cfg);

// read board ids defined for a given service
static int _toml_read_board_ids(service_t*, toml_array_t*);

// basic init
void hd_config_t_init(hd_config_t** cfg)
{
	debug("Initializing hd_config_t");
	*cfg = xmalloc(sizeof(hd_config_t));
	memset(*cfg, 0, sizeof(hd_config_t));
}

#ifdef _FREE_IF_NOT_NULL
#error "_FREE_IF_NOT_NULL is already defined, rename it!"
#endif
#define _FREE_IF_NOT_NULL(value)											\
	if (value != NULL)														\
{																			\
	xfree(value);															\
	value = NULL;															\
}

void hd_config_t_free(hd_config_t** cfg)
{
	if (*cfg == NULL)
	{
		return;
	}

	size_t num_services = 0;
	size_t idx_service = 0;
	size_t idx_board_id = 0;
	service_t* current_service = NULL;

	// scoop defines
	_FREE_IF_NOT_NULL((*cfg)->env_content_magic);
	_FREE_IF_NOT_NULL((*cfg)->env_name_magic);
	_FREE_IF_NOT_NULL((*cfg)->env_name_scoop_board_id);
	_FREE_IF_NOT_NULL((*cfg)->env_name_scoop_ip);
	_FREE_IF_NOT_NULL((*cfg)->env_name_scoop_job_id);
	_FREE_IF_NOT_NULL((*cfg)->env_name_scoop_port);
	_FREE_IF_NOT_NULL((*cfg)->env_name_error_msg);

	for (idx_service = 0; idx_service < num_services; ++idx_service)
	{
		current_service = (*cfg)->services + idx_service;
		if (current_service->num_board_ids > 0)
		{
			for (idx_board_id = 0; idx_board_id < num_services; ++idx_board_id)
			{
				_FREE_IF_NOT_NULL(current_service->board_ids[idx_board_id]);
			}
			_FREE_IF_NOT_NULL(current_service->board_ids);
			current_service->num_board_ids = 0;
		}
	}

	_FREE_IF_NOT_NULL((*cfg)->services);

	// hagen daas defines
	// jobname fmt specifier having one string placeholder for the board_id
	_FREE_IF_NOT_NULL((*cfg)->scoop_jobname_prefix);

	_FREE_IF_NOT_NULL((*cfg)->scoop_job_user);

	// working directory into which slurm logs etc are being placed
	_FREE_IF_NOT_NULL((*cfg)->scoop_working_dir);

	xfree(*cfg);

	*cfg = NULL;
}

#undef _FREE_IF_NOT_NULL

// load hd_config_t from file
// (*cfg) has to be free'd by the caller.
int hd_config_t_load(hd_config_t** cfg)
{
	debug("Loading config file..");
	char* path_config = get_extra_conf_path("hagen_daas.toml");
	/* char* path_config = "hagen_daas.toml"; */

	const int err_buf_size = 1024;
	char err_buf[err_buf_size];
	memset(err_buf, 0, err_buf_size);

	toml_table_t* root = NULL;
	FILE* buf_cfg_fd = NULL;

	hd_config_t_init(cfg);

	if (NULL == (buf_cfg_fd = fopen(path_config, "r")))
	{
		error("[hagen-daas] Error reading %s: %s", path_config, strerror(errno));
		return -1;
	}
	else
	{
		debug("[hagen-daas] Reading from %p", buf_cfg_fd);
		if (NULL ==
				(root = toml_parse_file(buf_cfg_fd, err_buf, err_buf_size)))
		{
			debug("[hagen-daas] Could not read %s: %s", path_config, err_buf);
		}
		else
		{
			debug("[hagen-daas] Successfully parsed file.");
		}
	}

	if (0 != _toml_table_to_hd_config(root, *cfg))
	{
		// TODO: handle error case
		error("[hagen-daas] Could not parse config file!");
		return -1;
	}
	else
	{
		debug("[hagen-daas] Successfully read config.");
	}

	if (buf_cfg_fd != NULL)
	{
		// we parsed the toml file -> clean up
		toml_free(root);
		fclose(buf_cfg_fd);
	}
	// TODO uncomment me
	// xfree(path_config);
	return 0;
}


#ifdef _TOML_READ_INT
#error "_TOML_READ_INT is already defined, rename it!"
#endif

#ifdef _TOML_READ_STR
#error "_TOML_READ_STR is already defined, rename it!"
#endif

// the following macros read `var_name` from root toml_table_t* and put it into
// `cfg->var_name` either as string or as integer.
#define _TOML_READ_INT(var_name)											\
	value = toml_raw_in(root, #var_name);									\
	if (value != NULL)														\
{																			\
	debug("[hagen-daas] Read: %s -> %s", #var_name, value);					\
	if (0 != toml_rtoi(value, &tmp_int64))									\
	{																		\
		error("[hagen-daas] error reading: %s", #var_name);					\
		return -1;															\
	}																		\
	else																	\
	{																		\
		cfg->var_name = tmp_int64;											\
	}																		\
	value = NULL;															\
	tmp_int64 = 0;															\
}																			\
else																		\
{																			\
	debug("[hagen-daas] taking default value for %s -> %d",					\
#var_name, hagen_daas_defaults.var_name);									\
	cfg->var_name = hagen_daas_defaults.var_name;							\
}

#define _TOML_READ_STR(var_name)											\
		value = toml_raw_in(root, #var_name);								\
	if (value != NULL)														\
{																			\
	if (0 != toml_rtos(value, &(cfg->var_name)))							\
	{																		\
		error("[hagen-daas] error reading: %s", #var_name);					\
		return -1;															\
	}																		\
	debug("[hagen-daas ] Read: %s -> %s", #var_name, cfg->var_name);		\
	value = NULL;															\
}																			\
	else																	\
{																			\
	debug("[hagen-daas] taking default value for %s -> %s",					\
#var_name, hagen_daas_defaults.var_name);									\
	cfg->var_name = xstrdup(hagen_daas_defaults.var_name);					\
}

int _toml_table_to_hd_config(toml_table_t* root, hd_config_t* cfg)
{
	char const* value = NULL;
	// temporary value for parsing
	int64_t tmp_int64;
	toml_array_t* services = NULL;

	_TOML_READ_STR(env_content_magic)
	_TOML_READ_STR(env_name_magic)
	_TOML_READ_STR(env_name_scoop_board_id)
	_TOML_READ_STR(env_name_scoop_ip)
	_TOML_READ_STR(env_name_scoop_job_id)
	_TOML_READ_STR(env_name_scoop_port)
	_TOML_READ_STR(env_name_error_msg)

	// hagen daas defines
	_TOML_READ_INT(scoop_port_lowest)

	// jobname prefix
	_TOML_READ_STR(scoop_jobname_prefix)

	_TOML_READ_STR(scoop_job_user)

	// working directory into which slurm logs etc are being placed
	_TOML_READ_STR(scoop_working_dir)

	// how many seconds does a compute job wait once the scoop has been started?
	_TOML_READ_INT(scoop_launch_wait_secs)

	// how many seconds is a started scoop job still considered pending
	_TOML_READ_INT(scoop_pending_secs)

	// time to wait till checking again if scoop is running in srun calls
	_TOML_READ_INT(srun_requeue_wait_period_secs)
	_TOML_READ_INT(srun_requeue_wait_num_periods)

	// time to wait for scoop launch job to appear in queue
	_TOML_READ_INT(scoop_launch_wait_period_secs)
	_TOML_READ_INT(scoop_launch_wait_num_periods)

	if (NULL != (services = toml_array_in(root, "service")))
	{
		_toml_read_services(services, cfg);
	}
	else
	{
		debug("[hagen-daas] no services defined, please define some!");
	}
	return 0;
}



#undef _TOML_READ_INT
#undef _TOML_READ_STR
#define _TOML_READ_INT(var_name)											\
	value = toml_raw_in(root, #var_name);									\
	if (value != NULL)														\
{																			\
	debug("[hagen-daas] Read: %s -> %s", #var_name, value);					\
	if (0 != toml_rtoi(value, &tmp_int64))									\
	{																		\
		error("[hagen-daas] error reading: %s", #var_name);					\
		return -1;															\
	}																		\
	else																	\
	{																		\
		new_service->var_name = tmp_int64;									\
	}																		\
	value = NULL;															\
	tmp_int64 = 0;															\
}																			\
else																		\
{																			\
	error("[hagen-daas] service %s does not define %s",						\
			name, #var_name);												\
	return -1;																\
}

#define _TOML_READ_STR(var_name)											\
	value = toml_raw_in(root, #var_name);									\
	if (value != NULL)														\
{																			\
	if (0 != toml_rtos(value, &(new_service->var_name)))					\
	{																		\
		error("[hagen-daas] error reading: %s->%s",							\
				name, #var_name);											\
	}																		\
	debug("[hagen-daas] Read: %s -> %s", #var_name, new_service->var_name);	\
	value = NULL;															\
}																			\
else																		\
{																			\
	error("[hagen-daas] service %s does not define %s",						\
			name, #var_name);												\
	return -1;																\
}

int _toml_read_services(toml_array_t* services, hd_config_t* cfg)
{
	toml_table_t* root = NULL;
	char const* name = NULL;
	char const* value = NULL;
	service_t* new_service = NULL;
	int num_keys = toml_array_nelem(services);
	int idx = 0;
	int64_t tmp_int64 = 0;  // used in templates below

	debug("[hagen-daas] Reading %d services..", num_keys);

	for (idx = 0; idx < num_keys; ++idx)
	{
		if (NULL == (root = toml_table_at(services, idx)))
		{
			error("[hagen-daas] Could not read service #%d", idx);
			return -1;
		}

		if (cfg->services == NULL)
		{
			cfg->services = xmalloc(sizeof(service_t));
			cfg->num_services = 1;
		}
		else
		{
			++(cfg->num_services);
			cfg->services = xrealloc(
					cfg->services,
					cfg->num_services * sizeof(service_t));
		}
		// the new service will now be the last element in the array
		new_service = cfg->services + cfg->num_services - 1;
		memset(new_service, 0, sizeof(service_t));

		debug("[hagen-daas] Reading new service..");
		_TOML_READ_STR(name)
		_TOML_READ_STR(script_path)
		_TOML_READ_STR(slurm_account)
		_TOML_READ_STR(slurm_partition)
		_TOML_READ_INT(num_cpus)
		_TOML_READ_INT(memory_in_mb)

		if (0 != _toml_read_board_ids(new_service,
									  toml_array_in(root, "board_ids")))
		{
			error("[hagen-daas] Could not read board ids for service %s",
					new_service->name);
			return -1;
		}
	}
	return 0;
}

int _toml_read_board_ids(service_t* service, toml_array_t* root)
{
	if (root == NULL)
	{
		return -1;
	}
	if (service->board_ids != NULL)
	{
		return -1;
	}
	service->num_board_ids = toml_array_nelem(root);

	debug("[hagen-daas] Reading %d board ids..", service->num_board_ids);

    size_t idx;

	service->board_ids = xmalloc(sizeof(char*) * service->num_board_ids);

	for (idx = 0; idx < service->num_board_ids; ++idx)
	{
		debug("[hagen-daas] Preparing to read board id #%d", idx+1);
		if (0 != toml_rtos(toml_raw_at(root, idx),
						   service->board_ids + idx))
		{
			error("[hagen-daas] Failed reading board id #%d", idx+1);
			return -1;
		}
		else
		{
			debug("[hagen-daas] Read board id: %s", service->board_ids[idx]);
		}
	}

	return 0;
}
#undef _TOML_READ_INT
#undef _TOML_READ_STR

const service_t* board_id_to_service(char const* board_id)
{
	size_t idx_service = 0,	// index services
		   idx_bid = 0;			// index board ids

	service_t const* service = NULL;

	if (hagen_daas_config == NULL)
	{
		return service;
	}

	for (idx_service = 0; idx_service < hagen_daas_config->num_services; ++idx_service)
	{
		service = hagen_daas_config->services + idx_service;

		for (idx_bid = 0; idx_bid < service->num_board_ids; ++idx_bid)
		{
			if (0 == strcmp(service->board_ids[idx_bid], board_id))
			{
				return service;
			}
		}
	}
	return NULL;
}

// vim: sw=4 ts=4 sts=4 noexpandtab tw=80
