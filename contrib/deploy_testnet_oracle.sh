#!/bin/bash
# ============================================================================
# DigiDollar Testnet Oracle + Seed Node - COMPLETE Deployment Script
# ============================================================================
# This script does EVERYTHING from a fresh Ubuntu server:
# 1. Installs all dependencies
# 2. Clones the correct branch
# 3. Builds DigiByte from source
# 4. Configures and starts the node
# 5. Creates Oracle_Seed wallet and oracle key
# 6. Starts Oracle 0 when the wallet key is authorized and DigiDollar is active
# 7. Starts mining blocks
# 8. Sets up systemd for auto-restart
#
# Usage: curl -sSL <url> | bash
#    or: ./deploy_testnet_oracle.sh
#
# Requirements: Ubuntu 22.04+ with sudo access
# ============================================================================

set -e
umask 077

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
CYAN='\033[0;36m'
NC='\033[0m' # No Color

print_banner() {
    echo -e "${CYAN}"
    echo "╔═══════════════════════════════════════════════════════════════╗"
    echo "║                                                               ║"
    echo "║     ██████╗ ██╗ ██████╗ ██╗██████╗  ██████╗ ██╗     ██╗       ║"
    echo "║     ██╔══██╗██║██╔════╝ ██║██╔══██╗██╔═══██╗██║     ██║       ║"
    echo "║     ██║  ██║██║██║  ███╗██║██║  ██║██║   ██║██║     ██║       ║"
    echo "║     ██║  ██║██║██║   ██║██║██║  ██║██║   ██║██║     ██║       ║"
    echo "║     ██████╔╝██║╚██████╔╝██║██████╔╝╚██████╔╝███████╗███████╗  ║"
    echo "║     ╚═════╝ ╚═╝ ╚═════╝ ╚═╝╚═════╝  ╚═════╝ ╚══════╝╚══════╝  ║"
    echo "║                                                               ║"
    echo "║              Testnet Oracle + Seed Node Deployer              ║"
    echo "║                                                               ║"
    echo "╚═══════════════════════════════════════════════════════════════╝"
    echo -e "${NC}"
}

print_banner

# Configuration
REPO_URL="https://github.com/DigiByte-Core/digibyte.git"
BRANCH="feature/digidollar-v1"
DIGIBYTE_DIR="$HOME/digibyte"
DATA_DIR="$HOME/.digibyte-testnet"
WALLET_NAME="Oracle_Seed"
TESTNET_NAME="testnet26"
TESTNET_P2P_PORT=12033
TESTNET_RPC_PORT=14026
TESTNET_GENESIS_HASH="0c9af936f28f7bd0e90c8f6235399063a026ed267bb53da398313b5d7aa55d82"
ORACLE_ID="${ORACLE_ID:-0}"

# Detect number of CPU cores for parallel compilation
NPROC=$(nproc 2>/dev/null || echo 2)
# Use fewer cores to avoid OOM on small VPS
if [ "$NPROC" -gt 2 ]; then
    MAKE_JOBS=$((NPROC - 1))
else
    MAKE_JOBS=$NPROC
fi

stop_configured_node() {
    local pid=""
    local cmdline=""

    "$DIGIBYTE_DIR/src/digibyte-cli" -testnet -datadir="$DATA_DIR" stop >/dev/null 2>&1 || true
    sleep 2

    if [ ! -f "$DATA_DIR/digibyted.pid" ]; then
        return 0
    fi

    pid="$(tr -cd '0-9' < "$DATA_DIR/digibyted.pid" 2>/dev/null || true)"
    if [ -z "$pid" ] || ! kill -0 "$pid" 2>/dev/null; then
        return 0
    fi

    cmdline="$(tr '\0' ' ' < "/proc/$pid/cmdline" 2>/dev/null || ps -p "$pid" -o args= 2>/dev/null || true)"
    if printf '%s' "$cmdline" | grep -F -q "digibyted" &&
       printf '%s' "$cmdline" | grep -F -q -- "-datadir=$DATA_DIR"; then
        kill "$pid" 2>/dev/null || true
        sleep 2
        if kill -0 "$pid" 2>/dev/null; then
            kill -9 "$pid" 2>/dev/null || true
        fi
    else
        echo -e "${YELLOW}PID file points to a process outside $DATA_DIR; leaving it running.${NC}"
    fi
}

echo -e "${BLUE}Configuration:${NC}"
echo "  Repository: $REPO_URL"
echo "  Branch: $BRANCH"
echo "  Install dir: $DIGIBYTE_DIR"
echo "  Data dir: $DATA_DIR"
echo "  Testnet: $TESTNET_NAME"
echo "  P2P port: $TESTNET_P2P_PORT"
echo "  RPC port: $TESTNET_RPC_PORT"
echo "  Build jobs: $MAKE_JOBS"
echo ""

# ============================================================================
# Step 1: System Update & Dependencies
# ============================================================================
echo -e "\n${YELLOW}[1/9] Installing system dependencies...${NC}"

if command -v apt-get &> /dev/null; then
    sudo apt-get update
    sudo apt-get install -y \
        build-essential \
        libtool \
        autotools-dev \
        automake \
        pkg-config \
        bsdmainutils \
        python3 \
        libssl-dev \
        libevent-dev \
        libboost-dev \
        libboost-system-dev \
        libboost-filesystem-dev \
        libboost-test-dev \
        libboost-thread-dev \
        libsqlite3-dev \
        libzmq3-dev \
        libminiupnpc-dev \
        libnatpmp-dev \
        libcurl4-openssl-dev \
        git \
        curl \
        ufw
    echo -e "${GREEN}Dependencies installed.${NC}"
elif command -v yum &> /dev/null; then
    sudo yum groupinstall -y "Development Tools"
    sudo yum install -y \
        libtool \
        openssl-devel \
        libevent-devel \
        boost-devel \
        sqlite-devel \
        zeromq-devel \
        miniupnpc-devel \
        libcurl-devel \
        git \
        curl
    echo -e "${GREEN}Dependencies installed (yum).${NC}"
else
    echo -e "${RED}Unsupported package manager. Please install dependencies manually.${NC}"
    exit 1
fi

# ============================================================================
# Step 2: Clone Repository
# ============================================================================
echo -e "\n${YELLOW}[2/9] Cloning DigiByte repository...${NC}"

if [ -d "$DIGIBYTE_DIR" ]; then
    echo "Directory exists, updating..."
    cd "$DIGIBYTE_DIR"
    git fetch origin
    git checkout "$BRANCH"
    git pull origin "$BRANCH"
else
    echo "Cloning fresh repository..."
    git clone "$REPO_URL" "$DIGIBYTE_DIR"
    cd "$DIGIBYTE_DIR"
    git checkout "$BRANCH"
fi

echo -e "${GREEN}Repository ready on branch: $BRANCH${NC}"

# ============================================================================
# Step 3: Build DigiByte
# ============================================================================
echo -e "\n${YELLOW}[3/9] Building DigiByte (this may take 10-30 minutes)...${NC}"

cd "$DIGIBYTE_DIR"

# Run autogen if configure doesn't exist
if [ ! -f "./configure" ]; then
    echo "Running autogen.sh..."
    ./autogen.sh
fi

# Configure without GUI, tests, or bench for faster build
echo "Running configure..."
./configure \
    --without-gui \
    --disable-tests \
    --disable-bench \
    --with-incompatible-bdb \
    --enable-reduce-exports \
    CXXFLAGS="-O2"

# Build
echo "Building with $MAKE_JOBS parallel jobs..."
make -j$MAKE_JOBS

# Verify binaries
if [ ! -f "$DIGIBYTE_DIR/src/digibyted" ] || [ ! -f "$DIGIBYTE_DIR/src/digibyte-cli" ]; then
    echo -e "${RED}Error: Build failed. Binaries not found.${NC}"
    exit 1
fi

echo -e "${GREEN}Build complete!${NC}"

# ============================================================================
# Step 4: Configure Firewall
# ============================================================================
echo -e "\n${YELLOW}[4/9] Configuring firewall...${NC}"

# Allow SSH (important - don't lock yourself out!)
sudo ufw allow ssh

# Allow DigiByte testnet P2P port
sudo ufw allow "$TESTNET_P2P_PORT/tcp" comment 'DigiByte Testnet P2P'

# Enable firewall if not already enabled
sudo ufw --force enable

echo -e "${GREEN}Firewall configured. Port $TESTNET_P2P_PORT is open.${NC}"

# ============================================================================
# Step 5: Create Configuration
# ============================================================================
echo -e "\n${YELLOW}[5/9] Creating node configuration...${NC}"

install -d -m 700 "$DATA_DIR"
CONFIG_FILE="$DATA_DIR/digibyte.conf"

# Generate secure RPC password
RPC_PASSWORD=$(openssl rand -hex 32)

cat > "$CONFIG_FILE" << EOF
# ============================================
# DigiDollar Testnet Oracle Node Configuration
# Generated: $(date)
# ============================================

# Global settings (apply to all networks)
server=1
listen=1
discover=1

# DigiDollar
digidollar=1
txindex=1

# RPC Credentials (auto-generated)
rpcuser=digibyterpc
rpcpassword=$RPC_PASSWORD

# Performance
dbcache=512
maxconnections=125
maxuploadtarget=5000

# Disable Dandelion for oracle (direct broadcast needed)
dandelion=0

# Logging
debug=digidollar
debug=net
shrinkdebugfile=1
printtoconsole=0

# Testnet-specific settings
[test]
port=$TESTNET_P2P_PORT
rpcport=$TESTNET_RPC_PORT
rpcbind=127.0.0.1
rpcallowip=127.0.0.1
acceptnonstdtxn=1
EOF
chmod 600 "$CONFIG_FILE"

echo -e "${GREEN}Configuration created.${NC}"
echo "  Config file: $CONFIG_FILE"
echo "  RPC Password: (saved in config)"

# ============================================================================
# Step 6: Create Systemd Service
# ============================================================================
echo -e "\n${YELLOW}[6/9] Creating systemd service...${NC}"

sudo tee /etc/systemd/system/digibyte-testnet.service > /dev/null << EOF
[Unit]
Description=DigiByte Testnet Oracle Node
Documentation=https://github.com/DigiByte-Core/digibyte
After=network-online.target
Wants=network-online.target

[Service]
Type=forking
User=$USER
Group=$USER

ExecStart=$DIGIBYTE_DIR/src/digibyted -testnet -datadir=$DATA_DIR -daemon -pid=$DATA_DIR/digibyted.pid
ExecStop=$DIGIBYTE_DIR/src/digibyte-cli -testnet -datadir=$DATA_DIR stop

PIDFile=$DATA_DIR/digibyted.pid
Restart=on-failure
RestartSec=30
TimeoutStartSec=120
TimeoutStopSec=300

# Hardening
PrivateTmp=true
ProtectSystem=full
NoNewPrivileges=true
MemoryDenyWriteExecute=false

[Install]
WantedBy=multi-user.target
EOF

sudo systemctl daemon-reload
sudo systemctl enable digibyte-testnet

echo -e "${GREEN}Systemd service created and enabled.${NC}"

# ============================================================================
# Step 7: Start Node
# ============================================================================
echo -e "\n${YELLOW}[7/9] Starting DigiByte node...${NC}"

# Stop only the instance using this script's configured datadir.
stop_configured_node

# First try starting directly to catch any errors
echo "Testing direct startup..."
"$DIGIBYTE_DIR/src/digibyted" -testnet -datadir="$DATA_DIR" -daemon -pid="$DATA_DIR/digibyted.pid" 2>&1 || {
    echo -e "${RED}Direct startup failed. Checking debug log...${NC}"
    tail -50 "$DATA_DIR/$TESTNET_NAME/debug.log" 2>/dev/null || echo "No debug log yet"
    echo -e "${YELLOW}Trying to continue anyway...${NC}"
}

# Wait and check if it started
sleep 5
if "$DIGIBYTE_DIR/src/digibyte-cli" -testnet -datadir="$DATA_DIR" getblockchaininfo >/dev/null 2>&1; then
    echo -e "${GREEN}Node started successfully via direct method!${NC}"
    # Stop it so we can restart via systemd
    stop_configured_node
fi

# Now start via systemd
sudo systemctl start digibyte-testnet

echo "Waiting for node to initialize..."
sleep 10

# CLI shortcut
CLI="$DIGIBYTE_DIR/src/digibyte-cli -testnet -datadir=$DATA_DIR"

# Wait for RPC to be ready (up to 6 minutes for slow VPS)
for i in {1..180}; do
    if $CLI getblockchaininfo &>/dev/null; then
        echo -e "${GREEN}Node is ready!${NC}"
        break
    fi
    if [ $i -eq 180 ]; then
        echo -e "${RED}Node failed to start. Check logs: tail -f $DATA_DIR/$TESTNET_NAME/debug.log${NC}"
        exit 1
    fi
    echo "Waiting for RPC... ($i/180)"
    sleep 2
done

# ============================================================================
# Step 8: Create Wallet & Start Oracle
# ============================================================================
echo -e "\n${YELLOW}[8/9] Setting up wallet and oracle...${NC}"

# Create or load wallet
if $CLI listwallets 2>/dev/null | grep -q "$WALLET_NAME"; then
    echo "Wallet '$WALLET_NAME' exists, loading..."
    $CLI loadwallet "$WALLET_NAME" 2>/dev/null || true
else
    echo "Creating wallet '$WALLET_NAME'..."
    $CLI createwallet "$WALLET_NAME"
fi

# Get mining address
MINING_ADDRESS=$($CLI -rpcwallet="$WALLET_NAME" getnewaddress "mining" "bech32")
echo -e "${GREEN}Mining Address: ${BLUE}$MINING_ADDRESS${NC}"

# Save address
echo "$MINING_ADDRESS" > "$DATA_DIR/mining_address.txt"

# Ensure the wallet has an oracle key. If this creates a new key, send the
# returned pubkey to the maintainer and wait for chainparams before startoracle
# can run successfully on public networks.
ORACLE_KEY_STATUS=0
ORACLE_KEY_RESULT=$($CLI -rpcwallet="$WALLET_NAME" createoraclekey "$ORACLE_ID" 2>&1) || ORACLE_KEY_STATUS=$?
if [ "$ORACLE_KEY_STATUS" -eq 0 ]; then
    echo -e "${GREEN}Oracle key created/stored for slot $ORACLE_ID:${NC}"
    echo "$ORACLE_KEY_RESULT"
else
    if echo "$ORACLE_KEY_RESULT" | grep -q "Oracle key already exists"; then
        echo -e "${YELLOW}Oracle key already exists in wallet '$WALLET_NAME' for slot $ORACLE_ID.${NC}"
    else
        echo -e "${RED}Oracle key setup failed:${NC}"
        echo "$ORACLE_KEY_RESULT"
        exit 1
    fi
fi

# Start Oracle if DigiDollar is active. Fresh testnet26 nodes may need to sync
# and reach activation before startoracle is valid.
echo "Checking DigiDollar activation before starting Oracle $ORACLE_ID..."
DD_DEPLOYMENT_INFO=$($CLI getdigidollardeploymentinfo 2>/dev/null || true)
ORACLE_STARTED=0
if echo "$DD_DEPLOYMENT_INFO" | grep -q '"status"[[:space:]]*:[[:space:]]*"active"'; then
    ORACLE_STATUS=0
    echo -e "${YELLOW}Starting oracle from the wallet-stored key created by createoraclekey.${NC}"
    ORACLE_RESULT=$($CLI -rpcwallet="$WALLET_NAME" startoracle "$ORACLE_ID" 2>&1) || ORACLE_STATUS=$?
    ORACLE_STATUS=${ORACLE_STATUS:-0}
    echo "$ORACLE_RESULT"
    if [ "$ORACLE_STATUS" -ne 0 ] || ! echo "$ORACLE_RESULT" | grep -q '"success"[[:space:]]*:[[:space:]]*true'; then
        echo -e "${RED}Oracle did not start. Check the oracle ID, assigned key, wallet, and activation status.${NC}"
        exit 1
    fi
    echo -e "${GREEN}Oracle $ORACLE_ID started successfully.${NC}"
    ORACLE_STARTED=1
else
    echo -e "${YELLOW}DigiDollar is not active yet; oracle start skipped.${NC}"
    echo -e "${YELLOW}After activation, run: dgb -rpcwallet=$WALLET_NAME startoracle $ORACLE_ID${NC}"
fi

# ============================================================================
# Step 9: Generate Initial Blocks & Create Helper Scripts
# ============================================================================
echo -e "\n${YELLOW}[9/9] Generating blocks and creating helper scripts...${NC}"

# Try to generate blocks
echo "Generating initial blocks..."
$CLI generatetoaddress 10 "$MINING_ADDRESS" 2>/dev/null || echo "Note: Mining may require additional setup on testnet"

# Create helper scripts
mkdir -p "$HOME/bin"

# CLI shortcut
cat > "$HOME/bin/dgb" << EOF
#!/bin/bash
$DIGIBYTE_DIR/src/digibyte-cli -testnet -datadir=$DATA_DIR "\$@"
EOF
chmod +x "$HOME/bin/dgb"

# Mine blocks script
cat > "$HOME/bin/mine" << EOF
#!/bin/bash
BLOCKS=\${1:-1}
ADDRESS=\$(cat $DATA_DIR/mining_address.txt)
echo "Mining \$BLOCKS blocks to \$ADDRESS..."
$DIGIBYTE_DIR/src/digibyte-cli -testnet -datadir=$DATA_DIR generatetoaddress \$BLOCKS \$ADDRESS
EOF
chmod +x "$HOME/bin/mine"

# Status script
cat > "$HOME/bin/dgb-status" << EOF
#!/bin/bash
CLI="$DIGIBYTE_DIR/src/digibyte-cli -testnet -datadir=$DATA_DIR"

echo -e "\033[0;36m=== DigiByte Testnet Oracle Status ===\033[0m"
echo ""

echo -e "\033[0;33mBlockchain:\033[0m"
\$CLI getblockchaininfo 2>/dev/null | grep -E '"chain"|"blocks"|"headers"|"verificationprogress"' | sed 's/^/  /'
ACTUAL_GENESIS=\$(\$CLI getblockhash 0 2>/dev/null || true)
if [ "\$ACTUAL_GENESIS" = "$TESTNET_GENESIS_HASH" ]; then
    echo "  genesis: \$ACTUAL_GENESIS (testnet26 OK)"
else
    echo "  genesis: \$ACTUAL_GENESIS (EXPECTED testnet26 $TESTNET_GENESIS_HASH)"
fi

echo ""
echo -e "\033[0;33mNetwork:\033[0m"
\$CLI getnetworkinfo 2>/dev/null | grep -E '"connections"|"subversion"' | sed 's/^/  /'

echo ""
echo -e "\033[0;33mOracle:\033[0m"
\$CLI listoracle 2>/dev/null | head -40 | sed 's/^/  /' || echo "  Local oracle status not available"
\$CLI getoracles 2>/dev/null | head -40 | sed 's/^/  /' || true

echo ""
echo -e "\033[0;33mWallet ($WALLET_NAME):\033[0m"
echo "  Balance: \$(\$CLI -rpcwallet=$WALLET_NAME getbalance 2>/dev/null || echo 'N/A') DGB"
echo "  Mining Address: \$(cat $DATA_DIR/mining_address.txt 2>/dev/null || echo 'N/A')"

echo ""
echo -e "\033[0;33mSystem:\033[0m"
echo "  Service: \$(systemctl is-active digibyte-testnet)"
echo "  Uptime: \$(systemctl show digibyte-testnet --property=ActiveEnterTimestamp | cut -d= -f2)"
echo "  Memory: \$(ps -o rss= -p \$(cat $DATA_DIR/digibyted.pid 2>/dev/null) 2>/dev/null | awk '{printf "%.1f MB", \$1/1024}' || echo 'N/A')"
EOF
chmod +x "$HOME/bin/dgb-status"

# Logs script
cat > "$HOME/bin/dgb-logs" << EOF
#!/bin/bash
tail -f $DATA_DIR/$TESTNET_NAME/debug.log
EOF
chmod +x "$HOME/bin/dgb-logs"

# Add bin to PATH if not already
if ! grep -q 'HOME/bin' ~/.bashrc; then
    echo 'export PATH="$HOME/bin:$PATH"' >> ~/.bashrc
fi
export PATH="$HOME/bin:$PATH"

# Get server IP
SERVER_IP=$(curl -s ifconfig.me 2>/dev/null || curl -s icanhazip.com 2>/dev/null || echo "YOUR_SERVER_IP")

# ============================================================================
# Print Summary
# ============================================================================
echo ""
echo -e "${GREEN}╔═══════════════════════════════════════════════════════════════╗${NC}"
echo -e "${GREEN}║           DEPLOYMENT COMPLETE!                                ║${NC}"
echo -e "${GREEN}╚═══════════════════════════════════════════════════════════════╝${NC}"
echo ""
echo -e "${BLUE}Node Status:${NC}"
$CLI getblockchaininfo 2>/dev/null | grep -E '"chain"|"blocks"|"headers"' | sed 's/^/  /' || echo "  Starting..."
echo ""
echo -e "${BLUE}Network:${NC}"
$CLI getnetworkinfo 2>/dev/null | grep '"connections"' | sed 's/^/  /' || echo "  Connecting..."
echo ""
echo -e "${BLUE}Configuration:${NC}"
echo "  Wallet: $WALLET_NAME"
echo "  Mining Address: $MINING_ADDRESS"
echo "  Oracle ID: $ORACLE_ID"
echo "  Server IP: $SERVER_IP"
echo ""
echo -e "${YELLOW}Quick Commands:${NC}"
echo "  dgb <command>     - Run digibyte-cli (e.g., dgb getinfo)"
echo "  dgb-status        - Show full node status"
echo "  dgb-logs          - Tail debug log"
echo "  mine [n]          - Mine n blocks (default: 1)"
echo ""
echo -e "${YELLOW}Service Management:${NC}"
echo "  sudo systemctl status digibyte-testnet"
echo "  sudo systemctl restart digibyte-testnet"
echo "  sudo systemctl stop digibyte-testnet"
echo ""
echo -e "${RED}═══════════════════════════════════════════════════════════════${NC}"
echo -e "${RED}IMPORTANT: Update your DNS records!${NC}"
echo -e "${RED}═══════════════════════════════════════════════════════════════${NC}"
echo ""
echo "  Point these domains to your server (A record, NO Cloudflare proxy):"
echo ""
echo -e "  ${CYAN}testnetseed.digibyte.io${NC}  →  ${GREEN}$SERVER_IP${NC}"
echo -e "  ${CYAN}oracle1.digibyte.io${NC}      →  ${GREEN}$SERVER_IP${NC}"
echo ""
if [ "$ORACLE_STARTED" -eq 1 ]; then
    echo -e "${GREEN}Your testnet oracle node is now running!${NC}"
else
    echo -e "${YELLOW}Your testnet node is running; oracle start is pending activation/key setup.${NC}"
fi
echo ""

# Reload bashrc for current session
source ~/.bashrc 2>/dev/null || true
