#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <slurm/slurm.h>

#include "src/common/read_config.h"
#include "src/common/xmalloc.h"
#include "src/common/xstring.h"

#include "toml.h"

#include "cerberus_config.h"
#include "cerberus_config_default.h"

// extract relevant configuration data from already parsed toml tree
static int _toml_table_to_crb_config(toml_table_t* root, crb_config_t* cfg);

// read a single service from a node (toml_node_walker)
static int _toml_read_partitions(toml_array_t*, crb_config_t* cfg);


static int _toml_read_int64_t(
    toml_table_t* root, char const* var_name, int64_t* target, int64_t* def_value);

static int _toml_read_str(
    toml_table_t* root, char const* var_name, char** target, char const* def_value);

static void _xfree_if_not_null(void** ptr);

// basic init
void crb_config_t_init(crb_config_t** cfg)
{
	debug("Initializing crb_config_t");
	*cfg = xmalloc(sizeof(crb_config_t));
	memset(*cfg, 0, sizeof(crb_config_t));
}

static void _xfree_if_not_null(void** ptr)
{
	if (*ptr != NULL) {
		xfree(*ptr);
		*ptr = NULL;
	}
}

void crb_config_t_free(crb_config_t** cfg)
{
	if (*cfg == NULL) {
		return;
	}

	size_t idx_part = 0;
	watched_partition_t* current_partition = NULL;

	for (idx_part = 0; idx_part < (*cfg)->num_partitions; ++idx_part) {
		current_partition = (*cfg)->partitions + idx_part;
		_xfree_if_not_null((void**) &(current_partition->name));
		_xfree_if_not_null((void**) &current_partition);
	}

	xfree(*cfg);

	*cfg = NULL;
}

// load crb_config_t from file
// (*cfg) has to be free'd by the caller.
int crb_config_t_load(crb_config_t** cfg)
{
	int rc = CERBERUS_PLUGIN_SUCCESS;
	debug("Loading config file..");
	char* path_config = get_extra_conf_path("cerberus.toml");

	const int err_buf_size = 1024;
	char err_buf[err_buf_size];
	memset(err_buf, 0, err_buf_size);

	toml_table_t* root = NULL;
	FILE* buf_cfg_fd = NULL;

	crb_config_t_init(cfg);

	buf_cfg_fd = fopen(path_config, "r");
	if (buf_cfg_fd != NULL) {
		debug("[cerberus] Reading from %p", buf_cfg_fd);
		root = toml_parse_file(buf_cfg_fd, err_buf, err_buf_size);
		if (NULL != root) {
			debug("[cerberus] Successfully parsed file.");
		} else {
			error("[cerberus] Could not read %s: %s", path_config, err_buf);
			rc = CERBERUS_PLUGIN_FAILURE;
			goto cleanup_load_config;
		}
	} else {
		error("[cerberus] Error reading %s: %s", path_config, strerror(errno));
		rc = CERBERUS_PLUGIN_FAILURE;
		goto cleanup_load_config;
	}

	if (_toml_table_to_crb_config(root, *cfg) == 0) {
		debug("[cerberus] Successfully read config.");
	} else {
		error("[cerberus] Could not parse config file!");
		rc = CERBERUS_PLUGIN_FAILURE;
		goto cleanup_load_config;
	}

cleanup_load_config:
	if (root != NULL) {
		// we parsed the toml file -> clean up
		toml_free(root);
	}
	if (buf_cfg_fd != NULL) {
		fclose(buf_cfg_fd);
	}
	xfree(path_config);
	return rc;
}

int _toml_table_to_crb_config(toml_table_t* root, crb_config_t* cfg)
{
	toml_array_t* partitions = NULL;
	partitions = toml_array_in(root, "partition");

	if (NULL != partitions) {
		_toml_read_partitions(partitions, cfg);
	} else {
		info("[cerberus] No partitions defined, please define some!");
	}
	return CERBERUS_PLUGIN_SUCCESS;
}

static int _toml_read_int64_t(
    toml_table_t* root, char const* var_name, int64_t* target, int64_t* def_value)
{
	char const* value = toml_raw_in(root, var_name);
	if (value != NULL) {
		debug("[cerberus] Read: %s -> %s", var_name, value);
		if (0 != toml_rtoi(value, target)) {
			error("[cerberus] error reading: %s", var_name);
			return CERBERUS_PLUGIN_FAILURE;
		}
	} else if (def_value != NULL) {
		*target = *def_value;
	} else {
		error("[cerberus] Could not read %s", var_name);
	}
	return CERBERUS_PLUGIN_SUCCESS;
}

static int _toml_read_str(
    toml_table_t* root, char const* var_name, char** target, char const* def_value)
{
	char const* value = toml_raw_in(root, var_name);
	char* tmp = NULL;
	if (value != NULL) {
		debug("[cerberus] Read: %s -> %s", var_name, value);
		if (0 == toml_rtos(value, &tmp)) {
			*target = xstrdup(tmp);
			free(tmp);
		} else {
			error("[cerberus] error reading: %s", var_name);
			return CERBERUS_PLUGIN_FAILURE;
		}
	} else if (def_value != NULL) {
		*target = xstrdup(def_value);
	} else {
		error("[cerberus] Could not read %s", var_name);
        return CERBERUS_PLUGIN_FAILURE;
	}
	return CERBERUS_PLUGIN_SUCCESS;
}

int _toml_read_partitions(toml_array_t* partitions, crb_config_t* cfg)
{
	toml_table_t* root = NULL;
	watched_partition_t* new_partition = NULL;
	int const num_keys = toml_array_nelem(partitions);
	int idx = 0;
	int rc = CERBERUS_PLUGIN_SUCCESS;
	int64_t tmp_int64 = 0;

	debug("[cerberus] Reading %d partitions..", num_keys);

	for (idx = 0; idx < num_keys; ++idx) {
		root = toml_table_at(partitions, idx);
		if (root == NULL) {
			error("[cerberus] Could not read partition #%d", idx);
			return CERBERUS_PLUGIN_FAILURE;
		}

		if (cfg->partitions == NULL) {
			cfg->partitions = xmalloc(sizeof(watched_partition_t));
			cfg->num_partitions = 1;
		} else {
			++(cfg->num_partitions);
			cfg->partitions =
			    xrealloc(cfg->partitions, cfg->num_partitions * sizeof(watched_partition_t));
		}
		// the new service will now be the last element in the array
		new_partition = cfg->partitions + cfg->num_partitions - 1;
		memset(new_partition, 0, sizeof(watched_partition_t));

		debug("[cerberus] Reading new partition..");
		rc = _toml_read_str(root, "name", &(new_partition->name), NULL);
		if (rc != CERBERUS_PLUGIN_SUCCESS) {
			return rc;
		}

		rc = _toml_read_int64_t(root, "num_allowed_jobs_per_user", &tmp_int64, NULL);
		if (rc != CERBERUS_PLUGIN_SUCCESS) {
			return rc;
		} else {
			new_partition->num_allowed_jobs_per_user = (uint32_t) tmp_int64;
		}
	}
	return CERBERUS_PLUGIN_SUCCESS;
}

#undef _TOML_READ_INT
#undef _TOML_READ_STR
