#!/usr/bin/env bash
# Build hayai.nro inside the pinned devkitPro container.
#
#   scripts/build.sh          configure (if needed) + build
#   scripts/build.sh clean    wipe the build directory first
set -euo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")/.."
ROOT="$(pwd)"
IMAGE="hayai-build"

if ! docker image inspect "$IMAGE" >/dev/null 2>&1; then
	echo ">> building $IMAGE (first run only, takes a few minutes)"
	docker build --platform linux/amd64 -t "$IMAGE" toolchain/
fi

if [[ "${1:-}" == "clean" ]]; then
	rm -rf build
fi

docker run --rm --platform linux/amd64 \
	-v "$ROOT:/src" -w /src \
	"$IMAGE" bash -euo pipefail -c '
		cmake -B build -GNinja \
			-DCMAKE_TOOLCHAIN_FILE=cmake/switch.cmake \
			-DCMAKE_BUILD_TYPE=Release
		ninja -C build
	'

echo
echo ">> $(ls -lh build/src/hayai.nro | awk '{print $5}')  build/src/hayai.nro"
