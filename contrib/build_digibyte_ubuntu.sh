#!/bin/bash
# ============================================================================
# DigiByte Complete Build Script for Ubuntu
# ============================================================================
# This script builds DigiByte from source including Qt GUI.
#
# Usage: ./build_digibyte_ubuntu.sh
#    or: curl -sSL <url> | bash
#
# Requirements: Ubuntu 22.04+ with sudo access
# ============================================================================

set -e

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
    echo "║     ██████╗ ██╗ ██████╗ ██╗██████╗ ██╗   ██╗████████╗███████╗ ║"
    echo "║     ██╔══██╗██║██╔════╝ ██║██╔══██╗╚██╗ ██╔╝╚══██╔══╝██╔════╝ ║"
    echo "║     ██║  ██║██║██║  ███╗██║██████╔╝ ╚████╔╝    ██║   █████╗   ║"
    echo "║     ██║  ██║██║██║   ██║██║██╔══██╗  ╚██╔╝     ██║   ██╔══╝   ║"
    echo "║     ██████╔╝██║╚██████╔╝██║██████╔╝   ██║      ██║   ███████╗ ║"
    echo "║     ╚═════╝ ╚═╝ ╚═════╝ ╚═╝╚═════╝    ╚═╝      ╚═╝   ╚══════╝ ║"
    echo "║                                                               ║"
    echo "║                    Ubuntu Build Script                        ║"
    echo "║                                                               ║"
    echo "╚═══════════════════════════════════════════════════════════════╝"
    echo -e "${NC}"
}

print_banner

# Configuration
REPO_URL="https://github.com/DigiByte-Core/digibyte.git"
BRANCH="feature/digidollar-v1"
DIGIBYTE_DIR="$HOME/digibyte"

# Detect number of CPU cores for parallel compilation
NPROC=$(nproc 2>/dev/null || echo 2)
# Use fewer cores to avoid OOM on small VPS
if [ "$NPROC" -gt 2 ]; then
    MAKE_JOBS=$((NPROC - 1))
else
    MAKE_JOBS=$NPROC
fi

echo -e "${BLUE}Configuration:${NC}"
echo "  Repository: $REPO_URL"
echo "  Branch: $BRANCH"
echo "  Install dir: $DIGIBYTE_DIR"
echo "  Build jobs: $MAKE_JOBS"
echo ""

# ============================================================================
# Step 1: System Update & Dependencies
# ============================================================================
echo -e "\n${YELLOW}[1/4] Installing system dependencies...${NC}"

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
        libqt5gui5 \
        libqt5core5a \
        libqt5dbus5 \
        qttools5-dev \
        qttools5-dev-tools \
        libqrencode-dev \
        git \
        curl
    echo -e "${GREEN}Dependencies installed.${NC}"
else
    echo -e "${RED}This script requires apt-get (Ubuntu/Debian). Please install dependencies manually.${NC}"
    exit 1
fi

# ============================================================================
# Step 2: Clone Repository
# ============================================================================
echo -e "\n${YELLOW}[2/4] Cloning DigiByte repository...${NC}"

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
# Step 3: Build DigiByte (with Qt GUI)
# ============================================================================
echo -e "\n${YELLOW}[3/4] Building DigiByte with Qt GUI (this may take 15-45 minutes)...${NC}"

cd "$DIGIBYTE_DIR"

# Run autogen if configure doesn't exist
if [ ! -f "./configure" ]; then
    echo "Running autogen.sh..."
    ./autogen.sh
fi

# Configure with Qt GUI enabled
echo "Running configure with Qt GUI..."
./configure \
    --with-gui=qt5 \
    --with-incompatible-bdb \
    --enable-reduce-exports \
    CXXFLAGS="-O2"

# Build
echo "Building with $MAKE_JOBS parallel jobs..."
make -j$MAKE_JOBS

# Verify binaries
MISSING_BINARIES=""
[ ! -f "$DIGIBYTE_DIR/src/digibyted" ] && MISSING_BINARIES="$MISSING_BINARIES digibyted"
[ ! -f "$DIGIBYTE_DIR/src/digibyte-cli" ] && MISSING_BINARIES="$MISSING_BINARIES digibyte-cli"
[ ! -f "$DIGIBYTE_DIR/src/qt/digibyte-qt" ] && MISSING_BINARIES="$MISSING_BINARIES digibyte-qt"

if [ -n "$MISSING_BINARIES" ]; then
    echo -e "${RED}Error: Build failed. Missing binaries:$MISSING_BINARIES${NC}"
    exit 1
fi

echo -e "${GREEN}Build complete!${NC}"

# ============================================================================
# Step 4: Create Helper Scripts
# ============================================================================
echo -e "\n${YELLOW}[4/4] Creating helper scripts...${NC}"

mkdir -p "$HOME/bin"

# digibyte-qt launcher
cat > "$HOME/bin/digibyte-qt" << EOF
#!/bin/bash
$DIGIBYTE_DIR/src/qt/digibyte-qt "\$@"
EOF
chmod +x "$HOME/bin/digibyte-qt"

# digibyted launcher
cat > "$HOME/bin/digibyted" << EOF
#!/bin/bash
$DIGIBYTE_DIR/src/digibyted "\$@"
EOF
chmod +x "$HOME/bin/digibyted"

# digibyte-cli launcher
cat > "$HOME/bin/digibyte-cli" << EOF
#!/bin/bash
$DIGIBYTE_DIR/src/digibyte-cli "\$@"
EOF
chmod +x "$HOME/bin/digibyte-cli"

# Add bin to PATH if not already
if ! grep -q 'HOME/bin' ~/.bashrc; then
    echo 'export PATH="$HOME/bin:$PATH"' >> ~/.bashrc
fi
export PATH="$HOME/bin:$PATH"

# ============================================================================
# Print Summary
# ============================================================================
echo ""
echo -e "${GREEN}╔═══════════════════════════════════════════════════════════════╗${NC}"
echo -e "${GREEN}║                    BUILD COMPLETE!                            ║${NC}"
echo -e "${GREEN}╚═══════════════════════════════════════════════════════════════╝${NC}"
echo ""
echo -e "${BLUE}Binaries built:${NC}"
echo "  $DIGIBYTE_DIR/src/digibyted"
echo "  $DIGIBYTE_DIR/src/digibyte-cli"
echo "  $DIGIBYTE_DIR/src/qt/digibyte-qt"
echo ""
echo -e "${YELLOW}Quick Commands (after restarting shell or running 'source ~/.bashrc'):${NC}"
echo "  digibyte-qt              - Launch GUI wallet"
echo "  digibyte-qt -testnet     - Launch GUI wallet (testnet)"
echo "  digibyted                - Start daemon"
echo "  digibyte-cli <command>   - CLI interface"
echo ""
echo -e "${YELLOW}Network Ports:${NC}"
echo "  Mainnet P2P:  12024    RPC: 14022"
echo "  Testnet P2P:  12033    RPC: 14026"
echo ""
echo -e "${YELLOW}Data Directories:${NC}"
echo "  Mainnet: ~/.digibyte"
echo "  Testnet: ~/.digibyte/testnet26"
echo ""
echo -e "${CYAN}To enable DigiDollar features, add to digibyte.conf:${NC}"
echo "  digidollar=1"
echo "  txindex=1"
echo ""
echo -e "${GREEN}Run 'source ~/.bashrc' to update your PATH, then start with 'digibyte-qt'${NC}"
echo ""
