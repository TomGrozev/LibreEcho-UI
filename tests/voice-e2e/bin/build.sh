#!/bin/bash
# Build the capture-path daemons and helpers inside a container.
set -euo pipefail
H="$(cd "$(dirname "$0")/.." && pwd)"
UI=${UI:-$(cd "$H/../.." && pwd)}
# arm64 by default: it is native on Apple Silicon, so it is faster AND
# behaves correctly.  Under emulated linux/amd64, signal delivery is broken
# (a caught SIGTERM kills the process outright), which makes every daemon look
# like it crashes on shutdown, an artifact of the emulator, not the code.
PLATFORM=${PLATFORM:-linux/arm64}
case "$PLATFORM" in
  linux/arm64) ARCHLIB=aarch64-linux-gnu ;;
  linux/amd64) ARCHLIB=x86_64-linux-gnu ;;
  *) echo "unsupported PLATFORM=$PLATFORM" >&2; exit 2 ;;
esac
IMAGE=libreecho-voice-harness:4

if ! docker image inspect "$IMAGE" >/dev/null 2>&1; then
  echo "== building toolchain image =="
  docker build --platform "$PLATFORM" -t "$IMAGE" -f - "$H" >/dev/null <<'DOCKER'
FROM debian:bookworm-slim
RUN apt-get update && apt-get install -y --no-install-recommends \
      build-essential libspeexdsp-dev ca-certificates \
      python3 curl \
  && rm -rf /var/lib/apt/lists/*
DOCKER
fi

echo "== building daemons ($PLATFORM) =="
docker run --rm --platform "$PLATFORM" -e "ARCHLIB=$ARCHLIB" \
  -v "$UI":/work -v "$H":/harness:ro -w /work "$IMAGE" bash -euo pipefail -c '
B=build/harness
# Objects are architecture-specific and make cannot see that the platform
# changed, so a stale tree links binaries for the wrong ISA.  Start clean.
rm -rf $B
mkdir -p /tmp/speex/lib /tmp/speex/include
ln -sf /usr/lib/$ARCHLIB/libspeexdsp.a /tmp/speex/lib/libspeexdsp.a
ln -sf /usr/include/speex /tmp/speex/include/speex

make -s BUILD=$B SPEEX_PREFIX=/tmp/speex $B/libreecho-micd

# Harness-only helpers: the fake codec tools micd execs.
gcc -O2 -Wall -Wextra -Wpedantic -std=c99 -Isrc -Isrc/adapter /harness/src/fake-tinycap.c src/adapter/mic_array_render.c -lm -o $B/fake-tinycap
gcc -O2 -Wall -Wextra -Wpedantic -std=c99 /harness/src/fake-tinymix.c -o $B/fake-tinymix
echo "-- built:"
ls -1 $B/libreecho-micd $B/fake-tinycap $B/fake-tinymix
'
