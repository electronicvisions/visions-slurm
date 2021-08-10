#!/bin/bash

# Extract the install type from the install base to destinguish between regular
# and testing install:
# /opt/slurm-skretch
# /opt/slurm-skretch-testing
#
# In our setups we then have the following directory structure:
# /opt/slurm{,-testing}/visions-slurm
# /opt/slurm{,-testing}/deployed
# and by default we want to install from visions-slurm to the corresponding
# deployed folder
if [ -z "${CLUSTERIZE_INSTALL_TYPE:-}" ]; then
    CLUSTERIZE_INSTALL_TYPE="$(realpath -P "${BASH_SOURCE[0]}" | awk -F '/' '{ print $3 }')"
fi

if [ -z "${CLUSTERIZE_INSTALL_TYPE}" ]; then
    echo -n "Could not deduce \$CLUSTERIZE_INSTALL_TYPE from " >&2
    echo "$(realpath -P "${BASH_SOURCE[0]}"), this should not happen!" >&2
fi

if [ -z "${SPACK_SETUP_ENV:-}" ]; then
  SPACK_SETUP_ENV="/opt/spack/share/spack/setup-env.sh"
fi

SINGULARITY_COMMANDS="$(dirname "$(realpath -P "${BASH_SOURCE[0]}")")/singularity-commands.sh"

if [ -z "${CLUSTERIZE_PREFIX:-}" ]; then
  CLUSTERIZE_PREFIX="/opt/${CLUSTERIZE_INSTALL_TYPE}"
fi

if [ -z "${HWDB_ROOT:-}" ]; then
  HWDB_ROOT="${CLUSTERIZE_PREFIX//slurm/hwdb}"
fi

if [ -z "${DEPLOY_ROOT:-}" ]; then
  DEPLOY_ROOT="${CLUSTERIZE_PREFIX}/deployed"
fi

GIT_REMOTE_DEPLOYMENT="review"

###############
# BOOKKEEPING #
###############

if [[ ! -v _slurmviz_exit_fns[@] ]]; then
  # bash only supports a single function for the exit trap, so we store all
  # functions to execute in an array an iterate over it
  _slurmviz_exit_fns=()

  _slurmviz_exit_trap() {
    for fn in "${_slurmviz_exit_fns[@]}"; do
      eval "${fn}"
    done
  }

  trap _slurmviz_exit_trap EXIT

  add_cleanup_step() {
    for fn in "$@"; do
      _slurmviz_exit_fns+=("${fn}")
    done
  }
fi

################
# /BOOKKEEPING #
################

get_config_opts() {
cat <<EOF | tr '\n' ' '
CFLAGS=-g
--prefix=${DEPLOY_ROOT}
--enable-pam
--with-hdf5=/opt/spack_views/visionary-slurmviz
--with-hwloc=/opt/spack_views/visionary-slurmviz
--with-json=/opt/spack_views/visionary-slurmviz
--with-libcurl=/opt/spack_views/visionary-slurmviz
--with-lz4=/opt/spack_views/visionary-slurmviz
--with-munge=/opt/spack_views/visionary-slurmviz
--with-readline=yes
--with-ssl=/opt/spack_views/visionary-slurmviz
--with-zlib=/opt/spack_views/visionary-slurmviz
EOF
}

systemd_slurm() {
  # Interact with slurm services. First argument can be:
  #
  # test: check for any running services
  #
  # start/stop/restart/status: Perform the given action for slurmdbd and
  # slurmctld (i.e., the services supposed to be run on slurmviz).
  case "$1" in
    "test")
      pgrep "slurmd@${CLUSTERIZE_INSTALL_TYPE}" && return 1
      pgrep "slurmctld@${CLUSTERIZE_INSTALL_TYPE}" && return 1
      pgrep "slurmdbd@${CLUSTERIZE_INSTALL_TYPE}" && return 1
      return 0
      ;;
    *)
      systemctl $1 slurmctld@${CLUSTERIZE_INSTALL_TYPE} || return 1
      systemctl $1 slurmdbd@${CLUSTERIZE_INSTALL_TYPE}  || return 1
      ;;
  esac
}

export_ldflags() {
  local rpaths
  rpaths=(
    "${DEPLOY_ROOT}/lib"
    "${HWDB_ROOT}/lib"
    "/opt/spack_views/visionary-slurmviz/lib"
  )

  # prepend all rpaths with '-rpath' and join by commas
  local merged_rpaths
  merged_rpaths="$(
    for rp in "${rpaths[@]}"; do
      echo "-rpath,${rp}"
    done | tr '\n' ','
  )"

  # remove last comma from ${merged_paths}
  export LDFLAGS="-Wl,${merged_rpaths::-1}${LDFLAGS:+,${LDFLAGS}}"
}

setup_compile_dependencies() {
  export CC=/opt/spack_views/visionary-dev-tools/bin/gcc
  export CXX=/opt/spack_views/visionary-dev-tools/bin/g++

  export_ldflags
}

setup_singularity() {
  test -f "${SINGULARITY_COMMANDS}" || { echo "Could not locate singularity-commands.sh" >&2; exit 1; }
  shopt -s expand_aliases
  if [ ! -z ${SINGULARITYENV_INSTALL_TYPE+x} ]; then
    echo -n "WARN: \$SINGULARITYENV_INSTALL_TYPE is already defined as: " >&2
    echo    "'${SINGULARITYENV_INSTALL_TYPE}', overwriting!" >&2j
  fi
  export SINGULARITYENV_INSTALL_TYPE="${CLUSTERIZE_INSTALL_TYPE}"
  source "${SINGULARITY_COMMANDS}" || { echo "Failed sourcing ${SINGULARITY_COMMANDS}" >&2; exit 1; }
}

# Paths that need to be purged from PATH as they will be appended by singularity.
# Otherwise binaries in these folders might shadow binaries in app-specific folders.
#
# Also purge paths with trailing slashes.
_purge_from_path=(
    "/usr/local/sbin/?"
    "/usr/local/bin/?"
    "/usr/sbin/?"
    "/usr/bin/?"
    "/sbin/?"
    "/bin/?"
)

escape_singularity_env() {
  # Because singularity is stupid and does not forward
  # SINGULARITYENV_SINGULARITYENV_LD_LIBRARY_PATH inside the container, we have
  # to work around:
  # We map SINGULARITYENV_<var> to SINGULARITYENV_CLUSTERIZEENV_<var> and call
  # restore_singularity_env inside the container to map back.
  # We do not want the current SINGULARITYENV_ variables to clutter the
  # environment of the inner process -> we need to unset them.
  # This can be disabled (for debugging purposes) via CLUSTERIZE_NO_CLEAN_SENV.
  FILE_AWK=$(mktemp)

  add_cleanup_step "rm '${FILE_AWK}'"

  cat >"${FILE_AWK}" <<EOF
BEGIN {
  verbose=$([ -n "${CLUSTERIZE_VERBOSE+x}" ] && echo 1 || echo 0)
  keep_singularity_env=$([ -n "${CLUSTERIZE_NO_CLEAN_SENV+x}" ] && echo 1 || echo 0)

  if (keep_singularity_env && verbose) {
    printf("# User requested _NOT_ clearing SINGULARITYENV_-variables because ")
    printf("CLUSTERIZE_NO_CLEAN_SENV was set.\n")
  }
}

function purge_path(path) {
    return gensub(/(^|:)($(IFS='|';
        echo "${_purge_from_path[*]//\//\\/}"))(:|$)/, "::", "g", path)
}

(\$1 ~ /^SINGULARITYENV_/) || (\$1 ~ /^PATH$/) || (\$1 ~ /^LD_LIBRARY_PATH$/) {
  var_name=\$1
  var_value=\$2

  if (verbose) {
    printf("echo \"# Escaping for nested singularity environments: %s\"\n", var_name)
  }
  if (var_name ~ /^PATH$/)
  {
    if (verbose) {
      printf("echo \"# Purging common directories from \$PATH.\n")
      printf("# They will be re-appended by singularity.\"\n")
    }
    # Purge the default folders but leave the colons (they will be removed in a next step)
    var_value=purge_path(var_value)
    # Because gawk does not support zero-width lookahead regexes, we need to
    # perform the substitution twice in order to catch all paths..
    var_value=purge_path(var_value)
    # eliminate multiple colons left in place
    gsub(/::+/, ":", var_value)
    # strip prefixed and trailing colons
    gsub(/(^:|:$)/, "", var_value)
    if (verbose)
    {
        printf("echo \"# After purging: %s\"\n", var_value)
    }
  }

  printf("export SINGULARITYENV_CLUSTERIZEENV_%s=\"%s\"\n", var_name, var_value)

  # Don't unset PATH/LD_LIBRARY_PATH here
  if (!keep_singularity_env && (\$1 ~ /^SINGULARITYENV_/)) {
    printf("unset %s\n", var_name)
  }
}
EOF
    source <(env | awk -F = -f "${FILE_AWK}")
}

restore_singularity_env() {
  # Because singularity is stupid and does not forward
  # SINGULARITYENV_SINGULARITYENV_LD_LIBRARY_PATH inside the container, we have
  # to work around:
  # We map SINGULARITYENV_<var> to SINGULARITYENV_CLUSTERIZEENV_<var> and call
  # restore_singularity_env inside the container to map back.
  FILE_AWK=$(mktemp)

  add_cleanup_step "rm '${FILE_AWK}'"

  cat >"${FILE_AWK}" <<EOF
BEGIN {
  verbose=$([ -n "${CLUSTERIZE_VERBOSE+x}" ] && echo 1 || echo 0)
}

\$1 ~ /^CLUSTERIZEENV_/ {
  var_name=\$1
  var_value=\$2
  if (verbose) {
    printf("echo \"# Restoring for nested environments: %s\"\n", var_name)
  }
  printf("unset %s\n", var_name)
  gsub(/^CLUSTERIZEENV_/, "", var_name)

  # LD_LIBRARY_PATH and PATH might have been modified by the container app, so
  # simply restoring the user environment would erase them.
  # Solution: prepend the user environment to the container-app environment.
  if ((var_name ~ /^PATH$/) || (var_name ~ /^LD_LIBRARY_PATH$/)) {
      gsub(/$/, ":\$" var_name, var_value)
  }

  printf("export %s=\"%s\"\n", var_name, var_value)
  if (verbose) {
    printf("echo \"# export %s=\"%s\"\"\n", var_name, var_value)
  }
}
EOF
    source <(env | awk -F = -f "${FILE_AWK}")
}

export_hwdb_env() {
  export CPATH=${HWDB_ROOT}/include:${HWDB_ROOT}/hwdb:${CPATH:+:${CPATH}}
  export LIBRARY_PATH=${HWDB_ROOT}/lib${LIBRARY_PATH:+:${LIBRARY_PATH}}
  export LD_LIBRARY_PATH=${HWDB_ROOT}/lib${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}
}

export_slurm_env() {
  export PATH=${DEPLOY_ROOT}/bin${PATH:+:${PATH}}
  # all exectuables are built via RPATH
}

export_spack_view() {
  # Set up slurm view $1
  local SVF="/opt/spack_views/$1"
  export PATH="${SVF}/bin${PATH:+:}${PATH}"
  export PYTHONHOME="${SVF}"
  export SPACK_PYTHON_BINARY="${SVF}/bin/python"
  export MANPATH="${SVF}/man:${SVF}/share/man${MANPATH:+:}${MANPATH}"
  export LIBRARY_PATH="${SVF}/lib:${SVF}/lib64${LIBRARY_PATH:+:}${LIBRARY_PATH}"
  export LD_LIBRARY_PATH="${SVF}/lib:${SVF}/lib64${LD_LIBRARY_PATH:+:}${LD_LIBRARY_PATH}"
  export TCLLIBPATH="${SVF}/lib${TCLLIBPATH:+:}${TCLLIBPATH}"
  export CPATH="${SVF}/include${CPATH:+:}${CPATH}"
  export C_INCLUDE_PATH="${SVF}/include${C_INCLUDE_PATH:+:}${C_INCLUDE_PATH}"
  export CPLUS_INCLUDE_PATH="${SVF}/include${CPLUS_INCLUDE_PATH:+:}${CPLUS_INCLUDE_PATH}"
  export PKG_CONFIG_PATH="${SVF}/lib/pkgconfig:${SVF}/lib64/pkgconfig:/usr/lib/x86_64-linux-gnu/pkgconfig${PKG_CONFIG_PATH:+:}${PKG_CONFIG_PATH}"
  export CMAKE_PREFIX_PATH=${SVF}${CMAKE_PREFIX_PATH:+:}${CMAKE_PREFIX_PATH}
}

export_slurm_conf() {
  # There is an existing slurm config (probably from the jessie cluster) that
  # will probably be mounted into singularity container at /etc/slurm -> work
  # around it by mounting to /etc/slurm-skretch and adjust SLURM_CONF
  # environment variable in container
  source <(find /etc -maxdepth 1 -type d -name "slurm-*" \
    | awk '{ printf("export SLURM_CONF=\"%s/slurm.conf\"\n", $0) }')
  # NOTE: There should only be one export line because only one slurm-*
  # directory is mounted in container!
}

check_git_branch_modifiable() {
  local rc
  # check if we are able to modify branches on the review remote site
  git push "${GIT_REMOTE_DEPLOYMENT}" -f HEAD:refs/heads/deployed/dummy
  rc=$?
  if [ ${rc} -eq 0 ]; then
    # deleting a branch in gerrit - while succeeding in the git repo - always
    # returns an internal server error -> ignore
    git push "${GIT_REMOTE_DEPLOYMENT}" :refs/heads/deployed/dummy
  fi
  return ${rc}
}

get_deployed_branchname() {
    echo -n "refs/heads/deployed/${CLUSTERIZE_INSTALL_TYPE}"
}
