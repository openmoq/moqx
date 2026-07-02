#!/usr/bin/env bash
# detect-platform.sh — Resolve the moxygen artifact platform string for this
# host (e.g. ubuntu-22.04-amd64, bookworm-arm64, macos-15-arm64).
#
# Sourced by setup-deps-tarball.sh and setup-deps-dev-artifact.sh so both
# use identical naming; can also be executed directly to print the platform.
#
# Ubuntu derivatives (Linux Mint, Pop!_OS, elementary, ...) report their own
# ID/VERSION_ID in /etc/os-release (e.g. linuxmint/22.3), so ID alone can't
# name an artifact. For those we consult ID_LIKE and map the Ubuntu base
# release via UBUNTU_CODENAME (or /etc/upstream-release/lsb-release).
# Debian and Debian derivatives map to the bookworm artifacts, matching how
# plain Debian is handled. Override with MOQX_PLATFORM if detection is wrong.

detect_platform() {
    local os arch
    os=$(uname -s)
    arch=$(uname -m)

    local darch="${arch/x86_64/amd64}"
    darch="${darch/aarch64/arm64}"

    if [[ "$os" == "Darwin" ]]; then
        local ver
        ver=$(sw_vers -productVersion | cut -d. -f1)
        echo "macos-${ver}-arm64"
        return 0
    fi

    if [[ "$os" != "Linux" ]]; then
        echo "Error: unsupported OS: $os" >&2
        return 1
    fi

    if [[ ! -f /etc/os-release ]]; then
        echo "Error: cannot detect Linux distro (no /etc/os-release)" >&2
        return 1
    fi

    local id id_like version_id ubuntu_codename
    id=$(. /etc/os-release && echo "${ID:-}")
    id_like=$(. /etc/os-release && echo "${ID_LIKE:-}")
    version_id=$(. /etc/os-release && echo "${VERSION_ID:-}")
    ubuntu_codename=$(. /etc/os-release && echo "${UBUNTU_CODENAME:-}")

    case "$id" in
        ubuntu)
            echo "ubuntu-${version_id}-${darch}"
            return 0
            ;;
        debian)
            echo "bookworm-${darch}"
            return 0
            ;;
    esac

    # Ubuntu derivative: VERSION_ID is the derivative's own (Mint 22.3), so
    # resolve the Ubuntu base release from the codename instead.
    if [[ " $id_like " == *" ubuntu "* ]]; then
        local base_ver=""
        case "$ubuntu_codename" in
            jammy) base_ver="22.04" ;;
            noble) base_ver="24.04" ;;
        esac
        if [[ -z "$base_ver" && -f /etc/upstream-release/lsb-release ]]; then
            base_ver=$(. /etc/upstream-release/lsb-release && echo "${DISTRIB_RELEASE:-}")
        fi
        if [[ -n "$base_ver" ]]; then
            echo "ubuntu-${base_ver}-${darch}"
            return 0
        fi
        echo "Error: Ubuntu derivative '$id' with unrecognized base codename '${ubuntu_codename:-unset}'." >&2
        echo "       Set MOQX_PLATFORM explicitly (e.g. MOQX_PLATFORM=ubuntu-22.04-${darch})." >&2
        return 1
    fi

    if [[ " $id_like " == *" debian "* ]]; then
        echo "bookworm-${darch}"
        return 0
    fi

    echo "Error: unsupported Linux distro: $id (ID_LIKE='${id_like}')" >&2
    echo "       Set MOQX_PLATFORM explicitly (e.g. MOQX_PLATFORM=ubuntu-22.04-${darch})." >&2
    return 1
}

# Print the platform when executed directly (not sourced).
if [[ "${BASH_SOURCE[0]}" == "${0}" ]]; then
    detect_platform
fi
