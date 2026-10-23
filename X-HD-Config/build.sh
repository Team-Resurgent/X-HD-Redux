#!/bin/bash
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# Where the RXDK VS Code extension keeps its SDK and tools; set RXDK_ROOT to override.
RXDK_ROOT="${RXDK_ROOT:-${HOME}/.var/app/com.visualstudio.code/data/rxdk}"

export RXDK_STAGED_SDK="${RXDK_ROOT}/sdk"
export RXDK_STAGED_TOOLS="${RXDK_ROOT}/tools"
export RXDK_LLVM="${RXDK_ROOT}/llvm/xbox-linux-x64"

CONFIG="${1:-Debug}"

echo "== Building X-HD-Config XBE [${CONFIG}] =="
"${RXDK_ROOT}/tools/Rxdk.Cli" build --project-root "${SCRIPT_DIR}" --configuration "${CONFIG}"

echo ""
echo "Build succeeded!"
echo "XBE location: ${SCRIPT_DIR}/out/${CONFIG}/Build/X-HD-Config/default.xbe"
echo "ISO location: ${SCRIPT_DIR}/out/${CONFIG}/XISO/X-HD-Config.iso"
