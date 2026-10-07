#!/bin/bash
# Finish Oracle Setup - Run this after the deploy script times out but node is running

DIGIBYTE_DIR="${DIGIBYTE_DIR:-/root/digibyte}"
DATA_DIR="${DATA_DIR:-/root/.digibyte-testnet}"
WALLET_NAME="${WALLET_NAME:-Oracle_Seed}"
ORACLE_ID="${ORACLE_ID:-0}"

CLI="sudo $DIGIBYTE_DIR/src/digibyte-cli -testnet -datadir=$DATA_DIR"
WALLET_CLI="$CLI -rpcwallet=$WALLET_NAME"

echo "Checking node status..."
$CLI getblockchaininfo | head -5

echo ""
echo "Creating wallet..."
$CLI createwallet "$WALLET_NAME" 2>/dev/null || $CLI loadwallet "$WALLET_NAME" 2>/dev/null || echo "Wallet already loaded"

echo ""
echo "Getting mining address..."
ADDRESS=$($WALLET_CLI getnewaddress)
echo "Mining Address: $ADDRESS"
sudo bash -c "echo '$ADDRESS' > $DATA_DIR/mining_address.txt"

echo ""
echo "Checking DigiDollar activation before starting Oracle $ORACLE_ID..."
DD_DEPLOYMENT_INFO=$($CLI getdigidollardeploymentinfo 2>/dev/null || true)
ORACLE_STARTED=0
if echo "$DD_DEPLOYMENT_INFO" | grep -q '"status"[[:space:]]*:[[:space:]]*"active"'; then
    ORACLE_STATUS=0
    echo "Starting oracle from the wallet-stored key created by createoraclekey."
    ORACLE_RESULT=$($WALLET_CLI startoracle "$ORACLE_ID" 2>&1) || ORACLE_STATUS=$?
    echo "$ORACLE_RESULT"
    if [ "$ORACLE_STATUS" -ne 0 ] || ! echo "$ORACLE_RESULT" | grep -q '"success"[[:space:]]*:[[:space:]]*true'; then
        echo "Oracle did not start. Check the oracle ID, assigned key, wallet, and activation status."
        exit 1
    fi
    ORACLE_STARTED=1
else
    echo "DigiDollar is not active yet; oracle start skipped."
    echo "After activation, run: $WALLET_CLI startoracle $ORACLE_ID"
fi

echo ""
if [ "$ORACLE_STARTED" -eq 1 ]; then
    echo "Oracle status:"
    $CLI listoracle
    $CLI getoracles | head -40
else
    echo "Oracle status unavailable until DigiDollar is active and startoracle succeeds."
    $CLI getdigidollardeploymentinfo 2>/dev/null | head -20 || true
fi

echo ""
if [ "$ORACLE_STARTED" -eq 1 ]; then
    echo "Done! Oracle is running."
else
    echo "Done! Node and wallet are ready; oracle start is pending activation/key setup."
fi
echo "To mine blocks: $CLI generatetoaddress 1 $ADDRESS"
