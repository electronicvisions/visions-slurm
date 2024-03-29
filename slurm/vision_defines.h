#pragma once

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-variable"
// comma separated list (CSL) of all allocated ADC boards,
// Format as in HWDB e.g.
// B204532,B123456
static const char* vision_slurm_adcs_env_name = "SLURM_ADCS";

// CSL of all FPGA IPs. Format halco::hicann::v2::IPv4 e.g.
// 192.168.20.1,192.168.20.6
static const char* vision_slurm_fpga_ips_env_name = "SLURM_FPGA_IPS";

// CSL of all explicitly allocated HICANNs (all 8 HICANNs are allocated if user specified FPGA or Reticle)
// Format halco::hicann::v2::slurm_license(HICANNGlobal) e.g.
// W20H0,W20H1,W20H2,W20H3
static const char* vision_slurm_hicanns_env_name = "SLURM_HICANNS";

// mode of HICANN neighbor init. Modes are:
// DEFAULT: Init dirty neighbors
// SKIP: Don't init any neighbors
// FORCE: Init all neighbors
static const char* vision_slurm_hicann_init_env_name = "SLURM_HICANN_INIT";

// CSL of all user requested slurm resources
// includes automatically allocated master FPGA(12), does NOT include neighboring licenses
// Format halco::hicann::v2::slurm_license(FPGAGlobal,TriggerGlobal) e.g.
// W20F0,B204523,B203120,W20T0
static const char* vision_slurm_hardware_licenses_env_name = "SLURM_HARDWARE_LICENSES";

// CSL of all neighboring HICANNs
// Format halco::hicann::v2::slurm_license(HICANNGlobal) e.g.
// W20H0,W20H1,W20H2,W20H3
static const char* vision_slurm_neighbor_hicanns_env_name = "SLURM_NEIGHBOR_HICANNS";

// CSL of all slurm licenses related to neighboring HICANNs (FPGA licenses)
// Format halco::hicann::v2::slurm_license(FPGAGlobal) e.g.
// W20F0,W20F1,W20F2,W20F3
static const char* vision_slurm_neighbor_licenses_env_name = "SLURM_NEIGHBOR_LICENSES";

// IP + port of DLS boards which are automatically power cycled in prolog. Format e.g.
// 192.168.152.34,3
static const char* vision_slurm_powercycle_env_name = "SLURM_POWERCYCLE";

// string of YAML entries from hwdb of allocated resources
static const char* vision_slurm_hwdb_yaml_env_name = "SLURM_HWDB_YAML";

// subset of vision_slurm_hardware_licenses_env_name where licenses where marked as dirty prior to job execution
// Format halco::hicann::v2::slurm_license(FPGAGlobal) e.g.
// W20F3,W20F8,W20F12
static const char* vision_slurm_dirty_licenses_env_name = "SLURM_DIRTY_LICENSES";

// Path to directory containing blacklisting information
static const char* vision_slurm_defects_path_env_name = "SLURM_DEFECTS_PATH";

// Environmental names for quiggeldy
// Whether or not the fisch playback executor should use quiggeldy in auto mode
static char const* const vision_quiggeldy_enabled_env_name = "QUIGGELDY_ENABLED";
// IP/port to which the client should connectl/the server should bind
static char const* const vision_quiggeldy_ip_env_name = "QUIGGELDY_IP";
static char const* const vision_quiggeldy_port_env_name = "QUIGGELDY_PORT";
static char const* const vision_quiggeldy_partition_env_name = "QUIGGELDY_PARTITION";
// A user name that the client can set for itself if several clients access quiggeldy via the same
// user. For obvious reasons, this only works if munge is disabled, hence the name.
// If this environment variable is set, the client automatically disables munge
// support, even if it was compiled in.
static char const* const vision_quiggeldy_user_no_munge_env_name = "QUIGGELDY_USER_NO_MUNGE";

#pragma GCC diagnostic pop
