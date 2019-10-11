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

# if file does not exist, it will be generated
# NOTE: This corresponds to the location INSIDE the container!
MODULE_FILE_STORE="/run/slurm/current_modules.sh"

# OJB (03.12.2018 17:56:54):
# For unknown reasons, autoconf and automake do not appear in the
# visionary-slurmviz view and hence need to be loaded "manually" prior to
# compiling.
#
# See: https://gitviz.kip.uni-heidelberg.de/projects/symap2ic/work_packages/3025
spack_dependencies=(
  "autoconf"
  "automake"
  "visionary-slurmviz"
)


get_latest_version() {
  # Usage: get_latest_version <pkg-name>
  #
  # Get the latest version of a given package in the spack installation. This
  # takes into account compiler version, so if a package is available by two
  # compiler versions, the newer one is taken.
  FILE_AWK=$(mktemp)
  cat >"${FILE_AWK}" <<EOF
/^--/ {
  # \`spack find\` sorts installed specs by compiler, these lines start with
  # two dashes and we can hence identify the compiler name in the fourth field.
  compiler=\$4
}

/^[a-zA-Z]/ {
  # insert compiler name into spec name at appropriate position (i.e., prior to
  # specifying any variants)
  idx = match(\$1, /(\\+|\\~|$)/);
  printf("%s%%%s%s\\n", substr(\$1, 0, idx-1), compiler, substr(\$1, idx))
}
EOF

  spack find -v "$1" | awk -f "${FILE_AWK}"| sort -V | tail -n 1
  rm "${FILE_AWK}"
}

get_config_opts() {
cat <<EOF | tr '\n' ' '
CFLAGS=-g
--prefix=${DEPLOY_ROOT}
--enable-pam
--with-hdf5=$(spack location -i "$(get_latest_version hdf5)")
--with-hwloc=$(spack location -i "$(get_latest_version hwloc@:1.999.999)")
--with-json=$(spack location -i "$(get_latest_version json-c)")
--with-libcurl=$(spack location -i "$(get_latest_version curl)")
--with-lz4=$(spack location -i "$(get_latest_version lz4)")
--with-munge=$(spack location -i "$(get_latest_version munge)")
--with-readline=yes
--with-ssl=$(spack location -i "$(get_latest_version openssl)")
--with-zlib=$(spack location -i "$(get_latest_version zlib)")
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
    "/opt/spack_views/visionary-wafer/lib"
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
  # Extracts all dependencies needed for compiling (i.e., packages described in
  # ${spack_dependencies}[@]) from spack and stores all module calls in a
  # temporary script at ${MODULE_FILE_STORE}. We therefore only have to call
  # the slow `spack module loads`-command once for each dependency. Should the
  # dependencies (or the spack installation used) change, the module script has
  # to be deleted manually in order to trigger regeneration.

  local reset_x
  test -f "${SPACK_SETUP_ENV}" || { echo "Could not init spack, failing!" >&2; exit 1; }
  source "${SPACK_SETUP_ENV}"

  # for some reason, SPACK_SHELL does not get set correctly, so adjust manually --obreitwi, 25-07-18 17:48:04
  export SPACK_SHELL="bash"

  if [ ! -f "${MODULE_FILE_STORE}" ]; then
    if [ ! -d "$(dirname "${MODULE_FILE_STORE}")" ]; then
      echo "/run/${CLUSTERIZE_INSTALL_TYPE} does not exist yet!" \
           "Typically one of the" \
           "slurm{,ctl,db}d@skretch{,-testing}.services" \
           "has to be started for them to exist.." >&2
      exit 1
    fi
    {
      echo "# Current modules used by slurm in container."
      echo "# Delete this file and restart slurm to force regeneration!"
      for dep in "${spack_dependencies[@]}"; do
        spack module tcl loads -r "$(get_latest_version ${dep})"
      done
    } \
    | awk '($0 in lines == 0) { lines[$0]; print }' \
    > "${MODULE_FILE_STORE}"

    [ $? ] || { echo "Could not set up spack environment" >&2 ; exit 1; }
  fi

  # no debug output for module loading -> too much
  set +x
  if [[ -o xtrace ]]; then
    set +x
    reset_x=1
  fi
  source ${MODULE_FILE_STORE}
  (( reset_x == 1 )) && module list && set -x

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

escape_singularity_env() {
  # Because singularity is stupid and does not forward
  # SINGULARITYENV_SINGULARITYENV_LD_LIBRARY_PATH inside the container, we have
  # to work around:
  # We map SINGULARITYENV_<var> to SINGULARITYENV_CLUSTERIZEENV_<var> and call
  # restore_singularity_env inside the container to map back.
  # We do not want the current SINGULARITYENV_ variables to clutter the
  # environment of the inner process -> we need to unset them.
  # This can be disabled (for debugging purposes) via CLUSTERIZE_NO_CLEAN_SENV.
  source <(
  FILE_AWK=$(mktemp)
  finish() {
    rm "${FILE_AWK}"
  }
  trap finish EXIT
  cat >"${FILE_AWK}" <<EOF
BEGIN {
  verbose=$([ -n "${CLUSTERIZE_VERBOSE+x}" ] && echo 1 || echo 0)
  keep_singularity_env=$([ -n "${CLUSTERIZE_NO_CLEAN_SENV+x}" ] && echo 1 || echo 0)

  if (keep_singularity_env && verbose) {
    printf("# User requested _NOT_ clearing SINGULARITYENV_-variables because ")
    printf("CLUSTERIZE_NO_CLEAN_SENV was set.\n")
  }
}

(\$1 ~ /^SINGULARITYENV_/) || (\$1 ~ /^PATH$/) || (\$1 ~ /^LD_LIBRARY_PATH$/) {
  var_name=\$1
  var_value=\$2

  if (verbose) {
    printf("echo \"# Escaping for nested singularity environments: %s\"\n", var_name)
  }
  printf("export SINGULARITYENV_CLUSTERIZEENV_%s=\"%s\"\n", var_name, var_value)

  # Don't unset PATH/LD_LIBRARY_PATH here
  if (!keep_singularity_env && (\$1 ~ /^SINGULARITYENV_/)) {
    printf("unset %s\n", var_name)
  }
}
EOF
  env | awk -F = -f "${FILE_AWK}"
  )
}

restore_singularity_env() {
  # Because singularity is stupid and does not forward
  # SINGULARITYENV_SINGULARITYENV_LD_LIBRARY_PATH inside the container, we have
  # to work around:
  # We map SINGULARITYENV_<var> to SINGULARITYENV_CLUSTERIZEENV_<var> and call
  # restore_singularity_env inside the container to map back.
  source <(
  FILE_AWK=$(mktemp)
  finish() {
    rm "${FILE_AWK}"
  }
  trap finish EXIT
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
  env | awk -F = -f "${FILE_AWK}")
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
  export PYTHONHOME="${SVF}${PYTHONHOME:+:}${PYTHONHOME}"
  export MANPATH="${SVF}/man:${SVF}/share/man${MANPATH:+:}${MANPATH}"
  export LIBRARY_PATH="${SVF}/lib:${SVF}/lib64${LIBRARY_PATH:+:}${LIBRARY_PATH}"
  export LD_LIBRARY_PATH="${SVF}/lib:${SVF}/lib64${LD_LIBRARY_PATH:+:}${LD_LIBRARY_PATH}"
  export TCLLIBPATH="${SVF}/lib${TCLLIBPATH:+:}${TCLLIBPATH}"
  export CPATH="${SVF}/include${CPATH:+:}${CPATH}"
  export C_INCLUDE_PATH="${SVF}/include${C_INCLUDE_PATH:+:}${C_INCLUDE_PATH}"
  export CPLUS_INCLUDE_PATH="${SVF}/include${CPLUS_INCLUDE_PATH:+:}${CPLUS_INCLUDE_PATH}"
  export PKG_CONFIG_PATH="${SVF}/lib/pkgconfig:${SVF}/lib64/pkgconfig:/usr/lib/x86_64-linux-gnu/pkgconfig${PKG_CONFIG_PATH:+:}${PKG_CONFIG_PATH}"
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
