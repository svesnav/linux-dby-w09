#!/usr/bin/env bash
set -euo pipefail

# Static arm64 busybox for the initramfs, from Ubuntu's busybox-static
# package (noble), on any Ubuntu host without changing its apt setup.
#   fetch_busybox.sh OUTPUT
out="${1:?usage: fetch_busybox.sh OUTPUT}"
apt="$(mktemp -d /tmp/busybox-apt.XXXXXXXX)"
trap 'rm -rf -- "$apt"' EXIT
mkdir -p "$apt/lists/partial" "$apt/archives/partial"
for suite in noble noble-updates; do
    echo "deb [arch=arm64 signed-by=/usr/share/keyrings/ubuntu-archive-keyring.gpg]" \
        "http://ports.ubuntu.com/ubuntu-ports $suite main universe"
done > "$apt/sources.list"
opts=(-o Dir::Etc::SourceList="$apt/sources.list" -o Dir::Etc::SourceParts=/dev/null
      -o Dir::State::Lists="$apt/lists" -o Dir::Cache="$apt" -o Dir::Cache::Archives="$apt/archives" -o Dir::State::Status=/dev/null
      -o APT::Architecture=arm64 -o APT::Architectures=arm64 -o Debug::NoLocking=1)
apt-get "${opts[@]}" -qq update
(cd "$apt" && apt-get "${opts[@]}" -qq download busybox-static)
dpkg-deb -x "$apt"/busybox-static_*.deb "$apt/x"
install -D -m 0755 "$(find "$apt/x" -name busybox -type f | head -n 1)" "$out"
file "$out"
