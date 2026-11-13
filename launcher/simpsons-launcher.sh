#!/bin/bash
set -eu
launcher_script=$(readlink -f -- "$0")
launcher_dir=$(dirname -- "$launcher_script")
cd "$launcher_dir"

# A host executable under /run/host still uses the container's loader and
# libraries. Ask the container's launch service to run the whole application
# on the host instead, keeping Steam's app identity for Gaming Mode.
if [ "${SIMPSONS_LAUNCHER_HOST:-0}" != 1 ]; then
    if [ -f /.flatpak-info ] && command -v flatpak-spawn >/dev/null 2>&1; then
        host_args=(--host --env=SIMPSONS_LAUNCHER_HOST=1)
        for name in SteamAppId SteamGameId STEAM_COMPAT_APP_ID SteamDeck SteamOS STEAM_GAMEPADUI SteamGamepadUI; do
            if [ -n "${!name:-}" ]; then host_args+=("--env=$name=${!name}"); fi
        done
        exec flatpak-spawn "${host_args[@]}" "$launcher_script" "$@"
    fi
    if [ -f /run/host/os-release ]; then
        for client in steam-runtime-launch-client \
            /usr/lib/pressure-vessel/from-host/bin/steam-runtime-launch-client; do
            if command -v "$client" >/dev/null 2>&1; then
                export SIMPSONS_LAUNCHER_HOST=1
                exec "$client" --alongside-steam --directory="$launcher_dir" \
                    --pass-env=SIMPSONS_LAUNCHER_HOST --pass-env=SteamAppId \
                    --pass-env=SteamGameId --pass-env=STEAM_COMPAT_APP_ID \
                    --pass-env=SteamDeck --pass-env=SteamOS --pass-env=STEAM_GAMEPADUI --pass-env=SteamGamepadUI \
                    -- "$launcher_script" "$@"
            fi
        done
    fi
fi

# Releases carry their own Python and Qt. Source checkouts can use PySide6
# installed locally or under launcher/vendor.
if [ "${1:-}" = --modkit ] && [ -e "$launcher_dir/../.git" ] && \
        [ -f "$launcher_dir/modkit_app.py" ]; then
    # A checkout's editor should show its current source, even when an older
    # packaged launcher is kept here for Play. Release packages use the bundle.
    for candidate in python3 python3.13 python3.12 python3.11 python3.10; do
        if command -v "$candidate" >/dev/null 2>&1 && \
                "$candidate" -c 'from PySide6 import QtWidgets' >/dev/null 2>&1; then
            exec "$candidate" "$launcher_dir/launcher.py" "$@"
        fi
    done
fi
if [ -x "$launcher_dir/simpsons-launcher" ]; then
    exec "$launcher_dir/simpsons-launcher" "$@"
fi

version_ok() {
    "$1" -c 'import sys; sys.exit(0 if (3, 10) <= sys.version_info[:2] < (3, 15) else 1)' 2>/dev/null
}

pick_python() {
    for cand in python3 python3.13 python3.12 python3.11 python3.10; do
        exe="$(command -v "$cand" 2>/dev/null)" || continue
        if version_ok "$exe"; then
            echo "$exe"
            return 0
        fi
    done
    return 1
}

if ! PYEXE=$(pick_python); then
    printf '%s\n' 'The source launcher needs Python 3.10-3.14. Release downloads include Python and Qt.' >&2
    exit 1
fi
exec "$PYEXE" launcher.py "$@"
