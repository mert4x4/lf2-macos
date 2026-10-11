#!/bin/sh
# Checks the prerequisites, then builds LF2.app from the original installer.
#
#   ./install.sh                      # looks for LF2_v2.0a.exe here or in ~/Downloads
#   ./install.sh path/to/LF2_v2.0a.exe
#   ./install.sh path/to/installed/game-folder
set -eu
cd "$(dirname "$0")"

fail() {
    echo "install.sh: $1" >&2
    exit 1
}

[ "$(uname -s)" = "Darwin" ] || fail "this tool only runs on macOS"
[ "$(uname -m)" = "arm64" ] || fail "an Apple Silicon Mac is required"
command -v python3 >/dev/null || fail "python3 not found; install the Xcode command line tools: xcode-select --install"
for tool in clang objdump codesign afconvert; do
    command -v "$tool" >/dev/null || fail "$tool not found; run: xcode-select --install"
done
command -v ffmpeg >/dev/null || fail "ffmpeg not found; run: brew bundle   (or: brew install ffmpeg)"

input="${1:-}"
if [ -z "$input" ]; then
    for candidate in LF2_v2.0a.exe "$HOME/Downloads/LF2_v2.0a.exe"; do
        if [ -f "$candidate" ]; then
            input="$candidate"
            break
        fi
    done
fi
[ -n "$input" ] || fail "LF2_v2.0a.exe not found; pass the installer or a game folder as an argument"

./lf2port.py "$input" -o LF2.app
echo
echo "Start the game with:  open LF2.app"
echo "To install it, drag LF2.app into your Applications folder."
