#!/bin/bash -x

set -euo pipefail

# The symlink to this script should reside in /opt/slurm-TYPE/quiggeldy!

# Get the directory the symlink is in.
# -s: do not resolve symlinks
PATH_QUIGGELDY="$(dirname "$(realpath -s "$0")")"
PATH_SLURM="$(dirname "${PATH_QUIGGELDY}")"
PATH_HWDB="${PATH_SLURM/slurm/hwdb}"
TYPE_SLURM="$(basename "${PATH_SLURM}")"

if ! [[ "${TYPE_SLURM}" =~ ^slurm- ]]; then
    echo "# Cannot deduce slurm-install location from ${PATH_QUIGGELDY}!" >&2
    exit 1
fi

PATH_CONTAINER="${PATH_SLURM}/container"
PATH_INSTALL="${PATH_SLURM}/deployed"

pushd "${PATH_QUIGGELDY}"

CMD="singularity exec -B "${PATH_QUIGGELDY}" \
                      -B "${PATH_INSTALL}" \
                      -a visionary-dls \
                      ${PATH_CONTAINER}"

export CFLAGS_PREPEND='-D_FORTIFY_SOURCE=1 -fstack-protector-strong'
export CXXFLAGS_PREPEND='-D_FORTIFY_SOURCE=1 -fstack-protector-strong'

rpaths=(
  "${PATH_INSTALL}/lib"
  "/opt/spack_views/visionary-dls/lib"
)

# prepend all rpaths with '-rpath' and join by commas
merged_rpaths="$(
  for rp in "${rpaths[@]}"; do
    echo "-rpath,${rp}"
  done | tr '\n' ','
)"

# remove last comma from ${merged_paths}
export LDFLAGS="-Wl,${merged_rpaths::-1},-z,defs${LDFLAGS:+,${LDFLAGS}}"
if [ -z "${BUILD_ONLY:+x}" ]; then
    # ensure empty (ECM has to look away) 
    # rm -rv ./bin ./build ./lib ./.waf-* ./.symwaf2ic* || true

    # ensure waf repo exists
    if [ ! -d waf-repo ]; then
        ${CMD} git clone git@gitviz.kip.uni-heidelberg.de:waf.git waf-repo \
               -b symwaf2ic 
    fi

    pushd waf-repo
    ${CMD} git pull
    ${CMD} make
    popd
    cp -v waf-repo/waf .

    ${CMD} ./waf setup --project=haldls
    ${CMD} ./waf repos-update --repo-db-url=https://github.com/electronicvisions/projects
    ${CMD} ./waf configure --disable-confcache --build-profile=debug \
        --prefix "${PATH_INSTALL}"
fi

${CMD} ./waf build install --target quiggeldy

popd
