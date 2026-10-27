#!/bin/sh
# Packs minipsd as a Debian package, dist/libminipsd-dev_X.Y.Z_all.deb (CPack, CMakeLists.txt's
# rules), installs it, and builds tests/consumer against what it installed, by find_package and by
# pkg-config: a package that does not install, or is not found, is not published. Linux, with
# cmake, pkg-config and sudo; CI's linux-gcc job and the release run it.
#
#   sh tools/check_deb.sh
set -eu
root=$(cd "$(dirname "$0")/.." && pwd)
version=$(sed -nE 's/^#define MINIPSD_VERSION_(MAJOR|MINOR|PATCH) +([0-9]+)$/\2/p' "$root/include/minipsd.h" | paste -sd.)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

cmake -S "$root" -B "$work/package" -DMINIPSD_BUILD_TESTS=OFF
cpack --config "$work/package/CPackConfig.cmake" -G DEB
deb="$root/dist/libminipsd-dev_${version}_all.deb"
dpkg-deb -I "$deb"
dpkg-deb -c "$deb"
sudo apt-get install -y "$deb"

for mode in package pkgconfig; do
    cmake -S "$root/tests/consumer" -B "$work/consumer-$mode" -DMODE="$mode" -DMINIPSD_EXPECTED_VERSION="$version"
    cmake --build "$work/consumer-$mode"
    "$work/consumer-$mode/minipsd-consumer"
done
echo "$deb: installs, found by find_package and pkg-config"
