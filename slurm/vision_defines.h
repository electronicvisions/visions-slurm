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

// subset of vision_slurm_hardware_licenses_env_name where licenses where marked as dirty prior to job execution
// Format halco::hicann::v2::slurm_license(FPGAGlobal) e.g.
// W20F3,W20F8,W20F12
static const char* vision_slurm_dirty_licenses_env_name = "SLURM_DIRTY_LICENSES";
#pragma GCC diagnostic pop
