#!/usr/bin/env bash
set -euo pipefail

if [[ $# != 2 || ${ONEDRIVE_DISPOSABLE_PACKAGE_TEST:-} != 1 || $EUID != 0 ]]; then
    echo "Run only as root in a disposable container: ONEDRIVE_DISPOSABLE_PACKAGE_TEST=1 bash $0 SOURCE PACKAGE.deb" >&2
    exit 1
fi

source_dir=$(realpath "$1")
package=$(realpath "$2")
name=$(dpkg-deb --field "$package" Package)
version=$(dpkg-deb --field "$package" Version)
if [[ $name != onedrive-cpp ]]; then
    echo "Unexpected package: $name" >&2
    exit 1
fi
if dpkg-query -W -f='${db:Status-Status}' "$name" 2>/dev/null | grep -qx installed; then
    echo "Refusing to overwrite an existing onedrive-cpp installation" >&2
    exit 1
fi

temporary=$(mktemp -d)
trap 'rm -rf -- "$temporary"' EXIT
chmod 755 "$temporary"
export DEBIAN_FRONTEND=noninteractive
export QT_QPA_PLATFORM=offscreen
export XDG_RUNTIME_DIR="$temporary/runtime"
export HOME="$temporary/home"
mkdir -m 700 "$XDG_RUNTIME_DIR"
mkdir "$HOME"

python3 "$source_dir/tests/packaging/metadata.py" \
    "$source_dir" "$package" usr/share/doc/onedrive-cpp dpkg-deb
dpkg-deb --control "$package" "$temporary/control"
grep -Fx /etc/onedrive-cpp/onedrive-cpp.toml "$temporary/control/conffiles"

# A changed package default forces dpkg's conffile decision during the upgrade.
dpkg-deb --raw-extract "$package" "$temporary/previous"
sed -i "s/^Version: .*/Version: ${version}~lifecycle/" \
    "$temporary/previous/DEBIAN/control"
printf '\n# Previous package default for the lifecycle fixture\n' \
    >> "$temporary/previous/etc/onedrive-cpp/onedrive-cpp.toml"
dpkg-deb --build --root-owner-group "$temporary/previous" "$temporary/previous.deb"
apt-get -y --no-install-recommends install "$temporary/previous.deb"
config=/etc/onedrive-cpp/onedrive-cpp.toml
printf '\n# Locally modified configuration\n' >> "$config"
cp "$config" "$temporary/expected.toml"
apt-get -y --no-install-recommends -o Dpkg::Options::=--force-confold install "$package"
cmp "$config" "$temporary/expected.toml"
test "$(dpkg-query -W -f='${Version}' "$name")" = "$version"

application_version=$(python3 -c \
    'import json,sys; print(json.load(open(sys.argv[1]))["version-semver"])' \
    "$source_dir/vcpkg.json")
test "$(onedrive-cpp --version)" = "onedrive-cpp $application_version"
onedrive-cpp --help >/dev/null
onedrive-cpp transfer watch --help >/dev/null
man -L C -w onedrive-cpp | grep -E '/man/man1/onedrive-cpp[.]1([.]gz)?$'
man -L zh_CN.UTF-8 -w onedrive-cpp | grep -E '/zh_CN/man1/onedrive-cpp[.]1([.]gz)?$'
grep -F 'ExecStart="/usr/bin/onedrive-cpp" transfer watch' \
    /usr/lib/systemd/user/onedrive-cpp.service
systemd-analyze --user --man=no verify /usr/lib/systemd/user/onedrive-cpp.service
systemctl --user --no-reload enable onedrive-cpp.service
test -L "$HOME/.config/systemd/user/default.target.wants/onedrive-cpp.service"
systemctl --user --no-reload disable onedrive-cpp.service
test ! -L "$HOME/.config/systemd/user/default.target.wants/onedrive-cpp.service"
if [[ -e /usr/bin/onedrive-cpp-gui ]]; then
    onedrive-cpp-gui --version
    onedrive-cpp-gui --help >/dev/null
    desktop-file-validate /usr/share/applications/onedrive-cpp-gui.desktop
else
    test ! -e /usr/share/applications/onedrive-cpp-gui.desktop
    if dpkg-deb --field "$package" Depends | grep -qi qt6; then
        echo "CLI-only package unexpectedly depends on Qt" >&2
        exit 1
    fi
fi

apt-get -y remove "$name"
test ! -e /usr/bin/onedrive-cpp
test ! -e /usr/bin/onedrive-cpp-gui
cmp "$config" "$temporary/expected.toml"
apt-get -y purge "$name"
test ! -e "$config"
echo "Package install, fixture upgrade, configuration preservation, remove and purge passed"
