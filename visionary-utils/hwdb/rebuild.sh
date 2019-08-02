#!/bin/bash -x
#
# Rebuild hwdb bindings in the current directory. Using the corresponding
# container.
#
# Intended to be symlinked into the hwdb-root directory.
#

set -euo pipefail

# Get the directory the symlink is in.
# -s: do not resolve symlinks
PATH_HWDB="$(dirname "$(realpath -s "$0")")"

TYPE_HWDB="$(basename "${PATH_HWDB}")"

if ! [[ "${TYPE_HWDB}" =~ ^hwdb- ]]; then
    echo "# Cannot deduce slurm-install location from ${PATH_HWDB}!" >&2
    exit 1
fi

PATH_SLURM="${PATH_HWDB/hwdb/slurm}"
PATH_CONTAINER="${PATH_SLURM}/container"

pushd "${PATH_HWDB}"

CMD="singularity exec -B $PWD -a visionary-wafer ${PATH_CONTAINER}"

# ensure empty
rm -rv ./bin ./build ./lib ./.waf-* ./.symwaf2ic* || true

# ensure waf repo exists
if [ ! -d waf-repo ]; then
    ${CMD} git clone git@gitviz.kip.uni-heidelberg.de:waf.git waf-repo \
           -b symwaf2ic
fi

pushd waf-repo
${CMD} git pull
${CMD} make
popd

[ -f ./waf ] && rm -v ./waf
cp -v waf-repo/waf .

${CMD} ./waf setup --project=hwdb
${CMD} ./waf repos-update --repo-db-url=https://github.com/electronicvisions/projects
export CFLAGS_PREPEND='-D_FORTIFY_SOURCE=1 -fstack-protector-strong'
export CXXFLAGS_PREPEND='-D_FORTIFY_SOURCE=1 -fstack-protector-strong'

rpaths=(
  "${PATH_HWDB}/lib"
  "/opt/spack_views/visionary-wafer/lib"
)

# prepend all rpaths with '-rpath' and join by commas
merged_rpaths="$(
  for rp in "${rpaths[@]}"; do
    echo "-rpath,${rp}"
  done | tr '\n' ','
)"

# remove last comma from ${merged_paths}
export LDFLAGS="-Wl,${merged_rpaths::-1},-z,defs${LDFLAGS:+,${LDFLAGS}}"
${CMD} ./waf configure -v --disable-confcache --build-profile=debug build install --target hwdb4c --test-execall

popd
