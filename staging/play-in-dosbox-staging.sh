#!/usr/bin/env bash
# Starts Ultima III in DOSBox Staging with its HTTP API turned on, so Ultima III
# Assistant can read the game. The game's own files aren't touched: the config
# this writes is kept under the cache folder, beside assistant.conf from here.
#
# Usage: play-in-dosbox-staging.sh [game folder] [path to dosbox-staging]
#        --dry-run   print the config and the command, and start nothing
#
# The game folder and the emulator can also come from ULTIMA3_DIR and
# DOSBOX_STAGING. Without either, the usual places are searched.
set -euo pipefail

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
assistant_conf="$script_dir/assistant.conf"
dry_run=0
game=${ULTIMA3_DIR:-}
dosbox=${DOSBOX_STAGING:-}

die() {
    printf 'play-in-dosbox-staging: %s\n' "$1" >&2
    exit 1
}

usage() {
    # The comment block at the top of this file, without the shebang.
    awk 'NR > 1 && /^#/ { sub(/^# ?/, ""); print; next } NR > 1 { exit }' "${BASH_SOURCE[0]}"
}

positional=()
while (($# > 0)); do
    case $1 in
        -h | --help)
            usage
            exit 0
            ;;
        --dry-run) dry_run=1 ;;
        -*) die "unknown option $1 (try --help)" ;;
        *) positional+=("$1") ;;
    esac
    shift
done
((${#positional[@]} > 0)) && game=${positional[0]}
((${#positional[@]} > 1)) && dosbox=${positional[1]}
((${#positional[@]} > 2)) && die "too many arguments (try --help)"

# A folder holds the game if Sosaria's map is in it.
holds_game() {
    [[ -f "$1/SOSARIA.ULT" || -f "$1/sosaria.ult" ]]
}

if [[ -n $game ]]; then
    holds_game "$game" || die "no Ultima III in '$game' (SOSARIA.ULT is missing)"
else
    for candidate in \
        "$HOME/GOG Games/Ultima 3" \
        "$HOME/GOG Games/Ultima 3 Exodus" \
        "$HOME/GOG Games/Ultima 3 - Exodus" \
        "$HOME/Games/Ultima 3" \
        "$HOME/games/ultima3" \
        "${XDG_DATA_HOME:-$HOME/.local/share}/GOG Games/Ultima 3"; do
        if holds_game "$candidate"; then
            game=$candidate
            break
        fi
    done
    [[ -n $game ]] || die "couldn't find Ultima III; pass its folder as the first argument"
fi
game=$(cd -- "$game" && pwd)

conf_dir="${XDG_CACHE_HOME:-$HOME/.cache}/ultima3-assistant"
game_conf="$conf_dir/game.conf"

# The emulator: a binary, or the Flathub build.
flatpak_id=io.github.dosbox-staging
emulator=()
if [[ -n $dosbox ]]; then
    emulator=("$dosbox")
elif command -v dosbox-staging > /dev/null; then
    emulator=(dosbox-staging)
elif command -v dosbox > /dev/null; then
    emulator=(dosbox)
elif command -v flatpak > /dev/null && flatpak info "$flatpak_id" > /dev/null 2>&1; then
    # The sandbox needs to see the game and the config files.
    emulator=(flatpak run "--filesystem=$game" "--filesystem=$conf_dir"
              "--filesystem=$script_dir:ro" "$flatpak_id")
else
    die "DOSBox Staging not found; install it (flatpak install flathub $flatpak_id)
       or pass its dosbox-staging as the second argument"
fi

# Check it really is Staging 0.83 or later, which is what serves the API.
version_line=$("${emulator[@]}" --version 2> /dev/null | head -1 || true)
if [[ -n $version_line ]]; then
    version=$(printf '%s' "$version_line" | grep -oE '[0-9]+\.[0-9]+(\.[0-9]+)?' | head -1 || true)
    case $version_line in
        *[Ss]taging*) ;;
        *) die "'${emulator[*]}' is not DOSBox Staging ($version_line); the HTTP API is only in Staging" ;;
    esac
    if [[ -n $version ]] && [[ $(printf '%s\n0.83\n' "$version" | sort -V | head -1) != 0.83 ]]; then
        die "DOSBox Staging $version has no HTTP API; 0.83 or later is needed"
    fi
fi

mkdir -p -- "$conf_dir"
{
    echo "# Written by play-in-dosbox-staging.sh for '$game'. Edits are lost;"
    echo "# change assistant.conf, or your own DOSBox Staging config, instead."
    echo
    echo "[dosbox]"
    echo "machine = svga_s3"
    echo "memsize = 16"
    echo
    echo "[cpu]"
    echo "cputype = 386_slow"
    echo "cycles  = fixed 3000"
    echo
    echo "[autoexec]"
    echo "mount c \"$game\""
    # The game saves to cloud_saves in the GOG layout, so keep writing there.
    if [[ -d "$game/cloud_saves" ]]; then
        echo "mount c \"$game/cloud_saves\" -t overlay"
    fi
    echo "c:"
    echo "ultima3.com"
    echo "exit"
} > "$game_conf"

command=("${emulator[@]}" -conf "$assistant_conf" -conf "$game_conf")

if ((dry_run)); then
    printf 'game folder: %s\n' "$game"
    printf 'command:'
    printf ' %q' "${command[@]}"
    printf '\n\n%s:\n' "$game_conf"
    cat -- "$game_conf"
    exit 0
fi

# Run from the game's folder: that's how the assistant finds the game's files.
cd -- "$game"
exec "${command[@]}"
