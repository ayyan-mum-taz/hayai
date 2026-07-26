#!/usr/bin/env bash
# Push build/src/hayai.nro to a Switch running hbmenu's netloader.
#
# On the Switch: open hbmenu, press Y to start netloader, then:
#   scripts/deploy.sh 192.168.1.42
#
# stdout/stderr from the running .nro come back over this connection, so leave
# the terminal open -- that is where the latency telemetry lands.
set -euo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")/.."
ROOT="$(pwd)"

if [[ $# -lt 1 ]]; then
	echo "usage: $0 <switch-ip> [args-passed-to-nro...]" >&2
	exit 1
fi
ADDR="$1"; shift

if [[ ! -f build/src/hayai.nro ]]; then
	echo "build/src/hayai.nro not found - run scripts/build.sh first" >&2
	exit 1
fi

docker run --rm --platform linux/amd64 \
	-v "$ROOT:/src" -w /src \
	-p 28771:28771 \
	--entrypoint /opt/devkitpro/tools/bin/nxlink \
	hayai-build \
	-a "$ADDR" -s build/src/hayai.nro "$@"
