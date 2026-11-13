#!/bin/sh
set -eu
modkit_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
export PYINSTALLER_RESET_ENVIRONMENT=1
exec "$modkit_dir/simpsons-launcher.sh" --modkit "$@"
