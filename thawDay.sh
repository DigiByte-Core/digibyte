#!/usr/bin/env bash
# Run the Thaw Day rehearsal with separate test wallets and a test-only client.
set -euo pipefail
SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
if [[ "${1:-}" == "build" ]]; then
    shift
    exec bash "$SCRIPT_DIR/contrib/thawday/build-client.sh" "$@"
fi
exec python3 "$SCRIPT_DIR/contrib/thawday/rehearsal.py" "$@"
