#!/usr/bin/env bash
# Build a separate client. Never replace the release wallet or its source.
set -euo pipefail
TASK_ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"
TASK_CLIENT="$TASK_ROOT/builds/thawday-client"
TASK_BASE=e3ad0a4522410b77ebd46e1cf17c65d944fee25b
TASK_PATCH="$TASK_ROOT/contrib/thawday/client.patch"
if [[ -e "$TASK_CLIENT" ]]; then
    echo "The test checkout already exists at $TASK_CLIENT."
    echo "Keep it and its evidence. Use the existing build, or review that checkout before rebuilding it."
    exit 1
fi
git -C "$TASK_ROOT" worktree add -b test/thawday-client-rc2 "$TASK_CLIENT" "$TASK_BASE"
git -C "$TASK_CLIENT" apply --check "$TASK_PATCH"
git -C "$TASK_CLIENT" apply "$TASK_PATCH"
cd "$TASK_CLIENT"
./autogen.sh
CC=gcc CXX=g++ CFLAGS='-O2 -g1' CXXFLAGS='-O2 -g1' LDFLAGS='-Wl,--no-keep-memory' \
    ./configure --with-gui=qt5 --enable-wallet --disable-tests --disable-qt-tests \
    --disable-bench --disable-fuzz-binary --disable-shared --with-pic
make -j8 -C src digibyted digibyte-cli qt/digibyte-qt
python3 "$TASK_ROOT/contrib/thawday/record-build.py" "$TASK_CLIENT" "$TASK_PATCH"
echo "The separate test client is built. Run ./thawDay.sh from the release checkout."
