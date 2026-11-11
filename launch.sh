#!/bin/bash
# Example PortMaster-style launcher. Place next to the zenonia4/ runtime folder.
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
if [ -d "$HERE/zenonia4" ]; then
  GAMEDIR="$HERE/zenonia4"
elif [ -d "/mnt/mmc/ports/zenonia4" ]; then
  GAMEDIR="/mnt/mmc/ports/zenonia4"
elif [ -d "/roms/ports/zenonia4" ]; then
  GAMEDIR="/roms/ports/zenonia4"
else
  echo "zenonia4 runtime folder not found" >&2
  exit 1
fi
cd "$GAMEDIR" || exit 1
mkdir -p logs saves
. /etc/profile 2>/dev/null || true
export SDL_RENDER_DRIVER="${RDRV:-opengles2}"
export LD_LIBRARY_PATH="/usr/lib/mali:${LD_LIBRARY_PATH}"
chmod +x ./zenonia4 2>/dev/null || true
exec ./zenonia4 "$@" > logs/game.log 2>&1
