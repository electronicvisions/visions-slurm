#pragma once

#include <stdlib.h>

#include "cerberus_config.h"

// This file is to be included from cerberus_daas_config.c
// and only contains the default config parameters

crb_config_t const cerberus_defaults = {
	.partitions = NULL,
	.num_partitions = 0,
};
