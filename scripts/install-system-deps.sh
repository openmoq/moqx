#!/usr/bin/env bash
# install-system-deps.sh — install the system libraries moqx needs.
#
# Required in BOTH dependency modes: the prebuilt moxygen install ships
# folly/fizz/mvfst/proxygen statically, but its CMake config still resolves
# fmt/glog/gflags/... and folly transitively needs OpenSSL/Boost at link time.
#
# Optional helper — install the equivalents by hand if you prefer.
#
# Run it plain: `scripts/install-system-deps.sh`. It elevates the package-manager
# calls itself, so it works as root (containers), as a sudo-capable user, and on
# macOS — where Homebrew refuses to run under sudo at all.
#
# The -dev packages here overlap cmake/CheckSystemDeps.cmake. Kept in sync by
# hand: this script installs cmake, so it cannot read that list back.
set -e

SUDO=""
if [ "$(id -u)" -ne 0 ]; then
    if command -v sudo >/dev/null 2>&1; then
        SUDO=sudo
    elif [ "$(uname)" != "Darwin" ]; then
        echo "Installing packages needs root: re-run as root or install sudo." >&2
        exit 1
    fi
fi

# apt can hang past its own transfer timeouts: a dribbling mirror defeats the
# 30s read timeout, and a wedged https helper never fires it (the helper is
# what enforces it). Progress watchdog: healthy apt prints continuously, so
# kill the call after 2 minutes of output silence; hard-cap the whole call at
# 15 minutes as a backstop against a mirror that trickles forever.
apt_get() {
    local log pid rc size prev=-1 idle=0
    log=$(mktemp)
    $SUDO timeout -k 30 900 apt-get -o Acquire::Retries=3 -o Acquire::http::Timeout=30 -o Acquire::https::Timeout=30 "$@" >"$log" 2>&1 &
    pid=$!
    tail -f --pid="$pid" "$log" &
    while kill -0 "$pid" 2>/dev/null; do
        sleep 5
        size=$(stat -c %s "$log" 2>/dev/null || echo -1)
        if [ "$size" = "$prev" ]; then
            idle=$((idle + 5))
            if [ "$idle" -ge 120 ]; then
                echo "ERROR: apt-get made no progress for ${idle}s; aborting" >&2
                $SUDO kill "$pid" 2>/dev/null || true
                sleep 5
                $SUDO kill -9 "$pid" 2>/dev/null || true
                wait "$pid" 2>/dev/null || true
                rm -f "$log"
                return 124
            fi
        else
            prev=$size; idle=0
        fi
    done
    wait "$pid" && rc=0 || rc=$?
    rm -f "$log"
    return "$rc"
}

# reflect-cpp (a moqx dependency) needs CMake >= 3.23, newer than the 3.22 that
# e.g. Ubuntu 22.04 ships. Install a current CMake from PyPI when the distro's is
# too old, rather than requiring users to add a third-party apt repo.
ensure_recent_cmake() {
    local have min=3.23
    have="$(cmake --version 2>/dev/null | sed -nE 's/.*version ([0-9]+\.[0-9]+(\.[0-9]+)?).*/\1/p' | head -1)"
    # Already >= min? (portable version compare — works on apt and dnf distros)
    if [ -n "$have" ] && [ "$(printf '%s\n%s\n' "$min" "$have" | sort -V | head -1)" = "$min" ]; then
        return
    fi
    # Pin to the 3.x series: CMake 4.x drops compatibility shims the Meta-stack
    # (folly/proxygen/…) source build still relies on.
    echo "CMake ${have:-not found} is older than $min; installing a current CMake 3.x from PyPI..."
    if ! command -v pip3 >/dev/null 2>&1; then
        if command -v apt-get >/dev/null 2>&1; then apt_get install -y python3-pip
        elif command -v dnf >/dev/null 2>&1; then $SUDO dnf install -y python3-pip; fi
    fi
    # --break-system-packages: PEP-668 distros refuse system-wide pip installs
    # otherwise; older pips don't know the flag, hence the fallback. timeout:
    # PyPI is the other unbounded network fetch here (see apt_get).
    $SUDO timeout 600 pip3 install --upgrade 'cmake<4' --break-system-packages 2>/dev/null \
        || $SUDO timeout 600 pip3 install --upgrade 'cmake<4'
    hash -r
    echo "Using $(cmake --version | head -1)"
}

install_ubuntu() {
    echo "Installing dependencies for Ubuntu/Debian..."
    apt_get update
    apt_get install -y \
        build-essential cmake ninja-build git pkg-config ccache \
        libssl-dev libunwind-dev libgoogle-glog-dev libgflags-dev \
        libdouble-conversion-dev libevent-dev libsodium-dev libzstd-dev \
        libboost-dev libboost-context-dev libboost-filesystem-dev \
        libboost-program-options-dev libboost-regex-dev libboost-thread-dev \
        libfmt-dev zlib1g-dev libc-ares-dev libbrotli-dev python3 gperf \
        jq curl
    ensure_recent_cmake
}

install_fedora() {
    echo "Installing dependencies for Fedora/CentOS/RHEL..."
    $SUDO dnf install -y \
        gcc gcc-c++ make cmake git pkgconf-pkg-config ccache \
        openssl-devel libunwind-devel glog-devel gflags-devel \
        double-conversion-devel libevent-devel libsodium-devel libzstd-devel \
        boost-devel fmt-devel zlib-devel c-ares-devel brotli-devel python3 gperf \
        jq curl
    if ! command -v ninja &>/dev/null; then
        $SUDO dnf install -y ninja-build 2>/dev/null || \
            echo "WARNING: install ninja manually (pip install ninja)."
    fi
    ensure_recent_cmake   # RHEL/CentOS 8 ship cmake 3.20 (< 3.23)
}

install_macos() {
    echo "Installing dependencies for macOS..."
    brew install \
        cmake ninja ccache openssl@3 glog gflags double-conversion \
        libevent libsodium zstd boost fmt c-ares gperf brotli jq
    # Homebrew ships CMake 4.x; the from-source moxygen build needs the 3.x
    # series (same reason as the Linux pin above). Prebuilt-mode builds are fine.
    case "$(cmake --version 2>/dev/null | sed -nE 's/.*version ([0-9]+).*/\1/p' | head -1)" in
        4*) echo "WARNING: CMake 4.x detected — from-source moxygen builds need CMake 3.x:"
            echo "         pip3 install 'cmake<4'  (and ensure it precedes brew's on PATH)" ;;
    esac
}

if [[ "$(uname)" == "Darwin" ]]; then
    install_macos
elif [[ -f /etc/os-release ]]; then
    . /etc/os-release
    case "$ID" in
        ubuntu|debian) install_ubuntu ;;
        fedora|centos|rhel) install_fedora ;;
        *)
            case "$ID_LIKE" in
                *ubuntu*|*debian*) install_ubuntu ;;
                *fedora*|*rhel*)   install_fedora ;;
                *) echo "Unsupported distro: $ID — install deps manually (see BUILD.md)"; exit 1 ;;
            esac ;;
    esac
else
    echo "Unsupported operating system"; exit 1
fi

echo
echo "Done. Build moqx with:  scripts/configure.sh --moxygen prebuilt-with-fallback && scripts/build.sh"
