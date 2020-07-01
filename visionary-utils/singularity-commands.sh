#!/bin/bash

if [ -z "${CLUSTERIZE_INSTALL_TYPE:-}" ]; then
    echo "\$CLUSTERIZE_INSTALL_TYPE is not defined, this should not happen!" >&2
    return
fi

# first check for locally installed singularity 3.0+
if [ -f /usr/local/bin/singularity ]; then
    SINGULARITY_BIN="/usr/local/bin/singularity"
else
    SINGULARITY_BIN="/usr/bin/singularity"
fi
if [ -z "${CLUSTERIZE_CONTAINER:-}" ]; then
    CLUSTERIZE_CONTAINER="${CLUSTERIZE_PREFIX}/container"
fi

add_if_exists() {
    # Usage: add_if_exists <source> <target> <alternative>
    # Add `source` to container under `target` (if specified).
    # If `source` does not exist, try adding `alternative` (if it exists)
    # NOTE: add_if_exists omits the newline to allow for mount options!
    [ -d "$1" ] && echo -n "-B $1" && [ -n "$2" ] && echo -n ":$2"
    [ ! -d "$1" ] && [ -d "$3" ] \
        && echo -n "-B $3" && [ -n "$2" ] && echo -n ":$2"
}

# determine which singularity app to run the given command in
determine_singularity_app() {
    # currently, we run all slurm-related commands on slurmviz in
    # visionary-wafer and without any app on hosts.
    # We allow users to specify an app to allow interoperability between other
    # container based apps and singularity (e.g., meta-nmpm-software).
    if [ -n "${CLUSTERIZE_APP:-}" ]; then
        echo "--app ${CLUSTERIZE_APP}"
    fi
}

generate_singularity_cmd_prefix() {
(
echo "${SINGULARITY_BIN}"
# suppress warnings unless env variable is set
[ -z ${CLUSTERIZE_VERBOSE+x} ] && echo "-s"
) | tr \\n ' '
}

generate_singularity_cmd_suffix() {
(
if [ $UID -eq 0 ] && [ -f /usr/local/bin/singularity ]; then
    # Allow SUID mounts when running as root (i.e. for slurmd).
    # This option was introduced in singularity 3.0+.
    echo "--allow-setuid"
fi
# BEGIN bind options
cat <<EOF
$(add_if_exists "${CLUSTERIZE_PREFIX}" /opt/slurm)
$([ "${CLUSTERIZE_PREFIX}" != /opt/slurm ] && add_if_exists "${CLUSTERIZE_PREFIX}")
$(add_if_exists "${HWDB_ROOT}")
$(add_if_exists /run/mysqld)
$(add_if_exists /run/nscd)
$(add_if_exists "/var/lib/${CLUSTERIZE_INSTALL_TYPE}" /var/lib/slurm)
$(add_if_exists "/var/log/${CLUSTERIZE_INSTALL_TYPE}" /var/log/slurm)
$(add_if_exists "/run/${CLUSTERIZE_INSTALL_TYPE}" /run/slurm)
$(add_if_exists /run/xtables.lock)
-B /sys/fs/cgroup:/opt/cgroup
-B /etc/group
EOF
for stomount in {loh,scratch,wang,ley,fasthome}; do
    add_if_exists "/${stomount}"
    echo ""
done

determine_singularity_app

# backward-compatibility for hel
if [[ "$(hostname)" == helvetica* ]]; then
    if [[ "$CLUSTERIZE_INSTALL_TYPE" == *-testing ]]; then
        echo "-B /opt/munge-testing/var/run/munge:/run/munge"
    else
        echo "-B /opt/munge-skretch/var/run/munge:/run/munge"
    fi
elif [[ "$CLUSTERIZE_INSTALL_TYPE" == *-testing ]]; then
    # we are in the testing build, use the testing munge
    add_if_exists /run/munge-testing /run/munge /run/munge
    echo ""
else
    echo "-B /run/munge"
fi
# in order to not conflict with the old installation, we need to mount slurm
# configuration files to /etc/slurm-config
echo "-B ${DEPLOY_ROOT}/etc:/etc/slurm-config:ro"
# Add all libraries udevadm needs on the cluster nodes because that is where
# spikeys are attached.
# TODO: Re-enable
# if [[ "$(hostname)" = "HBPHost"* ]]; then
#     ldd "$(which udevadm)" | awk '( $3 ~ /^\/.*/ ) { print "-B", $3 }'
#     echo "-B /sbin/udevadm"
# fi
if [ "${PWD}" != "/" ]; then
    echo "-B ${PWD}"
fi
# END bind options

echo "${CLUSTERIZE_CONTAINER}"
) | tr \\n ' '
}

SINGULARITY_CMD_PREFIX="$(generate_singularity_cmd_prefix)"
SINGULARITY_CMD_SUFFIX="$(generate_singularity_cmd_suffix)"

export CMD_SEXEC="${SINGULARITY_CMD_PREFIX} exec ${SINGULARITY_CMD_SUFFIX}"
export CMD_SSHELL="${SINGULARITY_CMD_PREFIX} shell ${SINGULARITY_CMD_SUFFIX}"

alias sexec="${CMD_SEXEC}"
alias sshell="${CMD_SSHELL}"
