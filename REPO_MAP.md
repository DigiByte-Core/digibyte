# REPO_MAP.md — DigiByte Core v9.26

*Generated: 2026-02-12*

> This map covers **core DigiByte C++ code only**. DigiDollar subsystem (`src/digidollar/`, `src/oracle/`, `src/rpc/digidollar*`) is documented in `REPO_MAP_DIGIDOLLAR.md`. Third-party libs (leveldb, secp256k1, crc32c, minisketch, univalue) and the `depends/` directory are excluded.
>
> Legend: ⚠️ = contains DigiDollar-specific additions on top of base DGB code

---

## Source Files — src/ (Root-Level)

### src/addrdb.cpp / .h
- `CBanDB` (class) → serializes/deserializes ban list to `banlist.json` on disk
- `DumpPeerAddresses()` → writes `peers.dat` from AddrMan to disk
- `ReadFromStream()` → deserializes AddrMan peers from a data stream with format versioning

### src/addresstype.cpp / .h
- `CTxDestination` (variant) → variant type holding all address types (PKHash, ScriptHash, WitnessV0KeyHash, WitnessV0ScriptHash, WitnessV1Taproot, WitnessUnknown)
- `ExtractDestination()` → extracts address from scriptPubKey into CTxDestination variant
- `GetScriptForDestination()` → converts CTxDestination to the corresponding scriptPubKey
- `IsValidDestination()` → returns true if destination is not CNoDestination (i.e., a real address)
- `ToKeyID()` → converts PKHash/WitnessV0KeyHash to legacy CKeyID for key lookups

### src/addrman.cpp / .h
- `AddrMan` (class) → manages known network peer addresses with bucketed tried/new tables, eviction, and random selection
  - `Add()` → adds new addresses learned from peers, placing them in "new" buckets
  - `Good()` → marks an address as successfully connected, promoting it to "tried" table
  - `Select()` → randomly selects an address for connection attempt, weighted by recency
  - `GetAddr()` → returns addresses for `getaddr` P2P response, filtered by network reachability
  - `Attempt()` → records a connection attempt timestamp for retry backoff
  - `ResolveCollisions()` → resolves bucket collisions between new and tried table entries

### src/arith_uint256.cpp / .h
- `base_uint<BITS>` (class template) → arithmetic operations on unsigned big integers (add, sub, multiply, divide, shift, compare)
- `arith_uint256` (class) → 256-bit unsigned integer with full arithmetic for difficulty/target calculations
- `ArithToUint256()` / `UintToArith256()` → converts between arithmetic arith_uint256 and serializable uint256

### src/banman.cpp / .h
- `BanMan` (class) → manages IP/subnet ban list with persistence to disk
  - `Ban()` → bans a network address or subnet for specified duration
  - `Unban()` → removes a ban entry for address or subnet
  - `IsBanned()` → checks if address/subnet is currently banned
  - `DumpBanlist()` → persists current ban list to `banlist.json`

### src/base58.cpp / .h
- `EncodeBase58()` → encodes raw bytes to base58 string (no checksum)
- `DecodeBase58()` → decodes base58 string back to raw bytes
- `EncodeBase58Check()` → base58 encoding with 4-byte double-SHA256 checksum appended
- `DecodeBase58Check()` → decodes and verifies base58check string, stripping checksum
- ⚠️ `CDigiDollarAddress` (class) → DigiDollar-specific address encoding using 2-byte prefixes (DD mainnet, TD testnet, RD regtest) for P2TR addresses
- ⚠️ `EncodeDigiDollarAddress()` / `DecodeDigiDollarAddress()` → helper functions for DD address encode/decode

### src/bech32.cpp / .h
- `bech32::Encode()` → encodes data with HRP (human-readable part) to bech32/bech32m string for SegWit addresses
- `bech32::Decode()` → decodes bech32/bech32m string, returns HRP, data, and encoding type
- `bech32::LocateErrors()` → identifies character positions of errors in an invalid bech32 string

### src/bip324.cpp / .h
- `BIP324Cipher` (class) → implements BIP324 v2 P2P transport encryption using elliptic curve Diffie-Hellman
  - `Initialize()` → performs ECDH key exchange, derives session keys for send/receive ChaCha20-Poly1305 streams
  - `Encrypt()` → encrypts a P2P message with AEAD (authenticated encryption with associated data)
  - `Decrypt()` → decrypts and authenticates a received encrypted P2P message

### src/blockencodings.cpp / .h
- `CBlockHeaderAndShortTxIDs` (class) → compact block representation using short transaction IDs (BIP 152)
- `PartiallyDownloadedBlock` (class) → reconstructs full block from compact block + mempool transactions
  - `InitData()` → initializes from compact block, pre-fills transactions found in mempool
  - `FillBlock()` → completes block reconstruction with missing transactions received from peer
- `BlockTransactionsRequest` / `BlockTransactions` → request/response messages for missing compact block txns

### src/blockfilter.cpp / .h
- `GCSFilter` (class) → Golomb-Coded Set filter for BIP 157/158 compact block filters
  - `Match()` → tests if a single element may be in the filter (probabilistic, false positives possible)
  - `MatchAny()` → tests if any element from a set may be in the filter
- `BlockFilter` (class) → wraps GCSFilter with block hash and filter type metadata
- `BlockFilterTypeName()` / `BlockFilterTypeByName()` → converts between filter type enum and string name

### src/chain.cpp / .h
- `CBlockIndex` (class) → in-memory index entry for every known block: height, hash, PoW, timestamps, file position, algo
  - `GetBlockHash()` → returns block header hash
  - `GetBlockTime()` → returns block timestamp
  - `GetMedianTimePast()` → returns median of last 11 block timestamps (for time-based locktime)
  - `GetBlockAlgo()` → returns which of the 5 mining algorithms produced this block
- `CChain` (class) → represents the active chain as an ordered vector of CBlockIndex pointers
  - `SetTip()` → sets chain tip to given block index
  - `FindFork()` → finds most recent common ancestor between this chain and given block
  - `Contains()` → checks if a block index is part of this chain
- `CBlockFileInfo` (class) → tracks block file metadata (size, heights, timestamps, block/undo counts)
- `CDiskBlockIndex` (class) → serializable version of CBlockIndex for on-disk storage
- `GetBlockProof()` → calculates proof-of-work score for a block (per-algo or aggregate)
- `GetAlgoForBlockIndex()` → determines which of the 5 mining algorithms was used for a block
- `GetLocator()` → builds exponentially-spaced block locator for P2P synchronization

### src/chainparams.cpp / .h
- `CreateChainParams()` → factory that creates CChainParams for mainnet/testnet/signet/regtest
- `Params()` → returns the currently active chain parameters (singleton)
- `SelectParams()` → selects active chain (mainnet/testnet/signet/regtest) at startup

### src/checkpoints.cpp / .h
- `GetLastCheckpoint()` → returns the most recent hardcoded checkpoint block index for fast initial sync validation

### src/checkqueue.h
- `CCheckQueue<T>` (class) → thread-safe queue for parallelizing script verification across worker threads
- `CCheckQueueControl<T>` (class) → RAII controller that submits script checks and waits for all workers to complete

### src/clientversion.cpp / .h
- `FormatFullVersion()` → returns version string like "v9.26.0"
- `FormatSubVersion()` → returns P2P sub-version string like "/DigiByte:9.26.0/"
- `CLIENT_VERSION` → integer encoding of major.minor.build version

### src/coins.cpp / .h
- `Coin` (class) → a single unspent transaction output: CTxOut + height + coinbase flag
- `CCoinsView` (class) → abstract base interface for UTXO set access (get coin, check existence, get best block)
- `CCoinsViewBacked` (class) → CCoinsView with a fallback/parent view (layered cache pattern)
- `CCoinsViewCache` (class) → in-memory UTXO cache layer on top of CCoinsViewBacked
  - `GetCoin()` / `HaveCoin()` → retrieves or checks existence of a UTXO
  - `AddCoin()` / `SpendCoin()` → adds new UTXO or marks one as spent
  - `Flush()` → writes dirty cache entries to the parent view
  - `GetCacheSize()` → returns number of cached UTXO entries
- `CCoinsViewErrorCatcher` (class) → wraps a CCoinsView and translates LevelDB read errors to runtime exceptions
- `AddCoins()` → adds all outputs from a transaction to the UTXO cache
- `AccessByTxid()` → finds any UTXO from a given txid (scans outputs)

### src/compressor.cpp / .h
- `CompressScript()` → compresses standard scriptPubKey types (P2PKH, P2SH, P2PK) for compact UTXO storage
- `DecompressScript()` → decompresses stored script back to full scriptPubKey
- `CompressAmount()` / `DecompressAmount()` → variable-length amount encoding for UTXO database efficiency

### src/core_read.cpp
- `ParseScript()` → parses human-readable script string (e.g., "OP_DUP OP_HASH160 ...") into CScript
- `DecodeHexTx()` → deserializes hex-encoded raw transaction into CMutableTransaction
- `DecodeHexBlk()` → deserializes hex-encoded block into CBlock
- `DecodeHexBlockHeader()` → deserializes hex-encoded block header into CBlockHeader
- `SighashFromStr()` → converts sighash type string ("ALL", "NONE", etc.) to integer flag

### src/core_write.cpp
- `ValueFromAmount()` → converts satoshi CAmount to human-readable decimal string (e.g., 100000000 → "1.00000000")
- `FormatScript()` → converts CScript to human-readable opcode string
- `ScriptToAsmStr()` → converts script to assembly notation with optional sighash decoding
- `EncodeHexTx()` → serializes transaction to hex string
- `TxToUniv()` → converts transaction to JSON UniValue with full detail (inputs, outputs, witness, undo data)
- `ScriptToUniv()` → converts scriptPubKey to JSON with address, ASM, hex representations

### src/cuckoocache.h
- `CuckooCache::cache<Element>` (class) → concurrent cuckoo-hash-based cache for fast script verification signature lookups
- `CuckooCache::bit_packed_atomic_flags` (class) → thread-safe bit-packed flag array for cache occupancy tracking

### src/dandelion.cpp
- `CConnman::isDandelionInbound()` → checks if a peer is an inbound Dandelion++ relay
- `CConnman::setLocalDandelionDestination()` → selects an outbound peer as local Dandelion stem relay target
- `CConnman::getDandelionDestination()` → returns the Dandelion stem relay destination for a given peer
- `CConnman::insertDandelionEmbargo()` → sets an embargo timer before a Dandelion tx fluffs (broadcasts normally)
- `CConnman::DandelionShuffle()` → periodically re-randomizes Dandelion routing graph for privacy
- `CConnman::ThreadDandelionShuffle()` → background thread that triggers periodic Dandelion graph shuffles

### src/dbwrapper.cpp / .h
- `CDBWrapper` (class) → C++ wrapper around LevelDB for key-value storage (block index, UTXO set, chain state)
  - `Read()` / `Write()` → typed get/put operations with automatic serialization
  - `Exists()` → checks key existence without reading value
  - `Erase()` → removes a key from the database
  - `NewIterator()` → creates a database cursor for range scans
  - `WriteBatch()` → atomically writes a batch of operations
  - `IsEmpty()` → checks if database contains any entries
- `CDBBatch` (class) → accumulates write/erase operations for atomic batch commit
- `CDBIterator` (class) → forward-only cursor for scanning database key-value pairs

### src/deploymentinfo.cpp / .h
- `DeploymentName()` → returns human-readable name for a consensus deployment (e.g., "segwit", "taproot", "odo")
- `GetBuriedDeployment()` → looks up a BuriedDeployment enum from its string name

### src/deploymentstatus.cpp / .h
- `DeploymentActiveAfter()` → checks if a consensus deployment is active after a given block (BIP9 or buried)
- `DeploymentActiveAt()` → checks if a consensus deployment is active at a specific block
- `DeploymentEnabled()` → checks if a deployment is enabled in consensus params (not buried/disabled)

### src/digibyte-chainstate.cpp
- Standalone utility that loads and validates the blockchain database without full node functionality

### src/digibyte-cli.cpp
- Command-line RPC client that sends JSON-RPC requests to a running digibyted node

### src/digibyted.cpp
- Entry point for the DigiByte daemon (digibyted) — parses args, calls AppInit, runs event loop

### src/digibyte-tx.cpp
- Offline transaction creation/signing utility — build, modify, and sign raw transactions without a running node

### src/digibyte-util.cpp
- Offline utility for GRIND (vanity block header grinding) and other one-off operations

### src/digibyte-wallet.cpp
- Offline wallet utility — create, info, salvage, dump wallet files without a running node

### src/external_signer.cpp / .h
- `ExternalSigner` (class) → interface to HWI-compatible external hardware wallet signers
  - `Enumerate()` → lists connected hardware wallets via external signer process
  - `DisplayAddress()` → asks hardware device to display an address for verification
  - `GetDescriptors()` → retrieves wallet descriptors from hardware device
  - `SignTransaction()` → sends PSBT to hardware device for signing

### src/flatfile.cpp / .h
- `FlatFilePos` (struct) → position in a flat file: file number + byte offset (used for block/undo storage)
- `FlatFileSeq` (class) → manages sequence of numbered flat files (blk00000.dat, blk00001.dat, etc.)
  - `FileName()` → returns filesystem path for a given file number
  - `Allocate()` → allocates space in the current file, auto-rolls to next file when full
  - `Flush()` → syncs file data and/or metadata to disk

### src/hash.cpp / .h
- `CHash256` (class) → double-SHA256 hasher (DigiByte's standard hash: SHA256d)
- `CHash160` (class) → SHA256 + RIPEMD160 hasher (for address generation)
- `Hash()` → computes double-SHA256 of arbitrary data
- `Hash160()` → computes SHA256+RIPEMD160 hash for public key → address derivation
- `HashWriter` / `CHashWriter` (class) → streaming hasher that serializes objects into a hash computation
- `MurmurHash3()` → fast non-cryptographic hash for bloom filters
- `BIP32Hash()` → HMAC-SHA512 based key derivation for BIP32 HD wallets
- `TaggedHash()` → BIP340-style tagged hash (SHA256 with domain-separation tag)
- `SHA256Uint256()` → single SHA256 of a uint256 (used in Taproot)

### src/headerssync.cpp / .h
- `HeadersSyncState` (class) → state machine for headers-first synchronization with memory-efficient commitment tracking
  - `ProcessNextHeaders()` → validates and processes a batch of received headers during sync
  - `GetState()` → returns current sync phase (PRESYNC collecting commitments, REDOWNLOAD verifying)

### src/httprpc.cpp / .h
- `StartHTTPRPC()` → registers HTTP RPC request handlers on the HTTP server
- `InterruptHTTPRPC()` / `StopHTTPRPC()` → gracefully shuts down HTTP RPC
- `StartREST()` / `InterruptREST()` / `StopREST()` → starts/stops the REST API interface

### src/httpserver.cpp / .h
- `InitHTTPServer()` → initializes libevent-based HTTP server for RPC and REST
- `StartHTTPServer()` → launches HTTP server worker threads
- `HTTPRequest` (class) → represents a single HTTP request with methods to read body, write response, set headers
- `RegisterHTTPHandler()` → registers a URL prefix handler (e.g., "/rest/" for REST API)
- `GetQueryParameterFromUri()` → extracts query parameter value from URI string

### src/i2p.cpp / .h
- `i2p::Session` (class) → manages I2P SAM (Simple Anonymous Messaging) sessions for private P2P networking
  - `Connect()` → establishes connection to an I2P destination through SAM bridge
  - `Listen()` → accepts incoming I2P connections
  - `Accept()` → accepts a queued incoming I2P connection

### src/init.cpp / .h
- `AppInitMain()` → main node initialization: loads blockchain, starts networking, wallet, RPC, indexes
- `AppInitBasicSetup()` → signal handlers, locale, file limits setup
- `AppInitParameterInteraction()` → validates and resolves conflicts between command-line arguments
- `AppInitSanityChecks()` → checks crypto library integrity (ECC, random, etc.)
- `AppInitLockDataDirectory()` → acquires exclusive lock on data directory
- `AppInitInterfaces()` → initializes IPC/wallet interfaces
- `Interrupt()` → signals all subsystems to begin shutdown
- `Shutdown()` → orderly teardown of all subsystems (network, wallet, indexes, mempool, block storage)
- `SetupServerArgs()` → registers all command-line arguments with help text
- `StartIndexBackgroundSync()` → launches background sync threads for block filter, coinstats, tx indexes
- ⚠️ Contains DigiDollar initialization: oracle node startup, DD wallet setup, activation height checks

### src/key.cpp / .h
- `CKey` (class) → encapsulates a private ECDSA key (secp256k1)
  - `MakeNewKey()` → generates a new random private key (compressed or uncompressed)
  - `Sign()` → produces ECDSA signature (DER-encoded) for a message hash
  - `SignSchnorr()` → produces BIP340 Schnorr signature for a message hash
  - `SignCompact()` → produces recoverable compact ECDSA signature (for message signing)
  - `GetPubKey()` → derives the corresponding public key
  - `Derive()` → BIP32 child key derivation
  - `Negate()` → negates the private key (for Taproot key tweaking)
- `ECC_Start()` / `ECC_Stop()` → initializes/finalizes the secp256k1 elliptic curve context
- `ECC_InitSanityCheck()` → verifies ECC library works correctly on this platform

### src/key_io.cpp / .h
- `EncodeDestination()` → converts CTxDestination to human-readable address string (base58check or bech32)
- `DecodeDestination()` → parses address string into CTxDestination with error reporting
- `EncodeSecret()` / `DecodeSecret()` → WIF (Wallet Import Format) encoding/decoding for private keys
- `EncodeExtKey()` / `DecodeExtKey()` → BIP32 extended private key serialization (xprv...)
- `EncodeExtPubKey()` / `DecodeExtPubKey()` → BIP32 extended public key serialization (xpub...)
- `IsValidDestinationString()` → validates an address string without full decode

### src/keystore.cpp / .h
- `CKeyStore` (class) → virtual base interface for key storage providers
- `CBasicKeyStore` (class) → in-memory key store for keys, scripts, and watchonly addresses
  - `AddKey()` → stores a private key indexed by its public key ID
  - `HaveKey()` → checks if a private key is available
  - `GetKey()` → retrieves a private key by ID
  - `AddCScript()` → stores a redeemScript for P2SH
- `GetKeyForDestination()` → resolves a destination address to the signing key ID

### src/logging.cpp / .h
- `BCLog::Logger` (class) → global logging system with categories, levels, file/console output
  - `LogPrintStr()` → writes a formatted log message to file and/or console
  - `EnableCategory()` / `DisableCategory()` → toggles logging categories (net, mempool, validation, etc.)
  - `SetLogLevel()` → sets minimum log level (trace, debug, info, warning, error)
- `LogInstance()` → returns the singleton Logger
- `GetLogCategory()` → parses category name string to flag enum

### src/mapport.cpp / .h
- `StartMapPort()` → begins UPnP/NAT-PMP port mapping for incoming P2P connections
- `InterruptMapPort()` / `StopMapPort()` → stops port mapping threads

### src/merkleblock.cpp / .h
- `CPartialMerkleTree` (class) → partial Merkle tree proof (SPV proof) matching specific transactions
  - `ExtractMatches()` → validates proof and extracts matched transaction hashes
- `CMerkleBlock` (class) → block header + partial Merkle tree for SPV clients
- `BitsToBytes()` / `BytesToBits()` → bit vector conversion utilities for Merkle tree serialization

### src/net.cpp / .h
- `CConnman` (class) → manages all P2P network connections, message send/receive, peer lifecycle
  - `Start()` → starts networking threads (socket handler, open connections, message handler)
  - `Stop()` → disconnects all peers and stops networking threads
  - `ConnectNode()` → establishes outbound connection to a peer
  - `PushMessage()` → serializes and queues a P2P message for sending to a peer
  - `ForEachNode()` → iterates over all connected nodes with a callback
  - `DisconnectNode()` → disconnects a specific peer
  - `AddNode()` → adds a manual peer address to connect to
  - `GetNodeCount()` → returns count of connected peers by type (inbound/outbound/total)
  - `GetTotalBytesRecv()` / `GetTotalBytesSent()` → network traffic counters
  - Dandelion++ methods: see `src/dandelion.cpp`
- `CNode` (class) → represents a single connected peer with socket, version info, message queues
  - `GetAddrLocal()` → returns local address as seen by this peer
  - `IsInboundConn()` / `IsOutboundOrBlockRelayConn()` → connection direction checks
  - `IsAddrFetchConn()` → checks if connection is address-fetch only
- `V1Transport` (class) → legacy Bitcoin P2P transport with 4-byte magic header + length + checksum
- `V2Transport` (class) → BIP324 encrypted P2P transport with ChaCha20-Poly1305 AEAD
- `CNodeStats` (class) → snapshot of peer statistics for RPC display
- `Discover()` → discovers local network interfaces for address advertisement
- `GetListenPort()` → returns the P2P listen port
- `AddLocal()` → registers a local address for peer advertisement
- `GetLocalAddrForPeer()` → selects best local address to advertise to a given peer

### src/net_permissions.cpp / .h
- `NetPermissions` (class) → parses and manages per-peer permission flags (bloomfilter, relay, forcerelay, noban, mempool, download, addr)
- `NetWhitebindPermissions` / `NetWhitelistPermissions` → whitebind/whitelist permission sets from config

### src/net_processing.cpp / .h
- `PeerManager` (class) → high-level P2P message processing: validates messages, manages block/tx download, peer scoring
  - `Make()` → factory method creating the implementation
  - `ProcessMessage()` → dispatches and handles all incoming P2P messages (version, verack, inv, getdata, tx, block, headers, etc.)
  - `SendMessages()` → builds and sends outgoing P2P messages (inv, getdata, headers, ping, addr)
  - `Misbehaving()` → increments peer's misbehavior score, disconnects/bans at threshold
  - `RelayTransaction()` → announces a transaction to connected peers via inv messages
  - `CheckForStaleTipAndEvictPeers()` → detects stalled sync and evicts unproductive peers
  - `FetchBlock()` → requests a specific block from a peer
  - `RelayDandelionTransaction()` → relays transaction via Dandelion++ stem phase
  - `CheckDandelionEmbargoes()` → checks for expired Dandelion embargoes and fluffs transactions

### src/net_types.cpp / .h
- `SerializationTypeString()` → converts ban list serialization type to string name
- Ban list serialization helpers for JSON format

### src/netaddress.cpp / .h
- `CNetAddr` (class) → network address supporting IPv4, IPv6, Tor (v2/v3), I2P, CJDNS
  - `IsIPv4()` / `IsIPv6()` / `IsTor()` / `IsI2P()` / `IsCJDNS()` → network type checks
  - `IsRoutable()` → returns true if address is globally routable
  - `IsLocal()` → checks if address is localhost/loopback
  - `GetNetwork()` → returns network type enum
- `CSubNet` (class) → network address with subnet mask for ban/whitelist ranges
- `CService` (class extends CNetAddr) → network address + port number

### src/netbase.cpp / .h
- `LookupHost()` → DNS resolution of hostname to network addresses
- `Lookup()` → resolves host:port string to CService addresses
- `LookupNumeric()` → resolves numeric address (no DNS) to CService
- `ConnectSocketDirectly()` → establishes TCP connection with timeout
- `ConnectThroughProxy()` → connects via SOCKS5 proxy
- `Socks5()` → SOCKS5 protocol handshake implementation
- `SetProxy()` / `GetProxy()` → configures per-network proxy settings
- `SetNameProxy()` → sets DNS name resolution proxy
- `IsBadPort()` → checks if port is commonly used by non-P2P services (ISP blocking risk)
- `Proxy` (class) → proxy configuration (address + randomized credentials)
- `ReachableNets` (class) → tracks which network types are reachable for address relay filtering

### src/netgroup.cpp / .h
- `NetGroupManager` (class) → computes /16 network groups for peer diversity (using optional ASMap for AS-level grouping)

### src/noui.cpp / .h
- `noui_connect()` → connects non-interactive message handlers (daemon mode, no GUI)
- `noui_ThreadSafeMessageBox()` → logs UI messages to debug log instead of displaying dialog

### src/outputtype.cpp / .h
- `ParseOutputType()` → parses address type string ("legacy", "p2sh-segwit", "bech32", "bech32m")
- `FormatOutputType()` → converts OutputType enum to string
- `GetDestinationForKey()` → generates address for a public key using specified output type
- `GetAllDestinationsForKey()` → returns all possible address types for a key
- `AddAndGetDestinationForScript()` → imports script and returns address for specified output type

### src/pow.cpp / .h
- `GetNextWorkRequired()` → dispatches to correct difficulty algorithm version based on block height (V1→V4 progression)
- `GetNextWorkRequiredV1()` → original difficulty adjustment (pre-DigiShield, Bitcoin-inherited)
- `GetNextWorkRequiredV2()` → DigiShield v1 — per-algo difficulty with asymmetric response (faster decrease)
- `GetNextWorkRequiredV3()` → MultiShield (DigiShield v3) — improved per-algo real-time difficulty adjustment
- `GetNextWorkRequiredV4()` → MultiAlgo v2 — current difficulty algorithm with 5-algo MultiShield balancing
- `CalculateNextWorkRequired()` → core difficulty calculation: adjusts target based on actual vs expected timespan
- `InitialDifficulty()` → returns genesis difficulty target for each algo
- `CheckProofOfWork()` → validates that a block hash meets the required difficulty target
- `GetLastBlockIndexForAlgo()` → walks chain backwards to find previous block using same mining algorithm
- `GetLastBlockIndexForAlgoFast()` → optimized version using cached algo data
- `GetPoWAlgoHash()` → hashes block header using the correct algorithm (SHA256d, Scrypt, Groestl, Skein, Qubit/Odocrypt)
- `PermittedDifficultyTransition()` → validates difficulty change between consecutive blocks is within allowed range

### src/protocol.cpp / .h
- `CMessageHeader` (class) → P2P message header: 4-byte magic + command + payload size + checksum
- `CAddress` (class extends CService) → peer address with services bitmap + timestamp for address relay
- `CInv` (class) → inventory vector: type (tx, block, filtered block, compact block) + hash
- `OraclePriceMsg` (class) → ⚠️ P2P message wrapper for oracle price updates
- `OracleBundleMsg` (class) → ⚠️ P2P message wrapper for oracle price bundles
- `GetOracleDataMsg` (class) → ⚠️ P2P message for requesting oracle data
- `serviceFlagsToStr()` → converts service flags bitmap to human-readable string list
- `GetDesirableServiceFlags()` → returns minimum service flags required from peers
- `getAllNetMessageTypes()` → returns list of all known P2P message type strings

### src/psbt.cpp / .h
- `PartiallySignedTransaction` (class) → BIP 174 Partially Signed Bitcoin Transaction container
- `PSBTInput` / `PSBTOutput` (classes) → per-input/output PSBT metadata (scripts, keys, signatures, Taproot data)
- `SignPSBTInput()` → signs a single PSBT input using the given signing provider
- `FinalizePSBT()` → combines all partial signatures into final scriptSig/witness
- `FinalizeAndExtractPSBT()` → finalizes and extracts the complete signed transaction
- `CombinePSBTs()` → merges multiple PSBTs (e.g., from different signers) into one
- `CountPSBTUnsignedInputs()` → returns count of inputs that still need signatures
- `PSBTInputSigned()` → checks if an input has any signatures
- `UpdatePSBTOutput()` → adds HD key paths and scripts to a PSBT output
- `PrecomputePSBTData()` → precomputes sighash data for all PSBT inputs

### src/pubkey.cpp / .h
- `CPubKey` (class) → encapsulates a compressed/uncompressed secp256k1 public key (33 or 65 bytes)
  - `Verify()` → verifies ECDSA signature against this public key
  - `IsFullyValid()` → checks if key is a valid point on the curve
  - `Decompress()` → converts compressed key to uncompressed form
  - `Derive()` → BIP32 child public key derivation
  - `GetID()` → returns Hash160 of the public key (used as address)
  - `IsCompressed()` → checks if key is in compressed format
- `XOnlyPubKey` (class) → 32-byte x-only public key for BIP340 Schnorr / Taproot
  - `VerifySchnorr()` → verifies BIP340 Schnorr signature
  - `CheckTapTweak()` → verifies Taproot key tweak against internal key + merkle root
  - `CreateTapTweak()` → creates Taproot-tweaked keypair from internal key
- `CKeyID` (class) → Hash160 of a public key, used as key identifier for lookups
- `CExtPubKey` (class) → BIP32 extended public key (key + chain code + depth + fingerprint)

### src/random.cpp / .h
- `GetRandBytes()` → fills buffer with cryptographically secure random bytes (OS entropy + hardware RNG + ChaCha20 mixer)
- `GetRand<T>()` → returns uniformly distributed random number in [0, max)
- `GetRandHash()` → returns a random uint256
- `GetStrongRandBytes()` → performs slow high-entropy random generation (for key material)
- `FastRandomContext` (class) → fast non-cryptographic PRNG for performance-critical randomization (peer selection, shuffle)
  - `randbool()` / `rand32()` / `rand64()` / `randrange()` → various random value generators
- `RandAddEvent()` → mixes timing/hardware events into the random pool for additional entropy
- `RandomInit()` → initializes random subsystem, seeds from OS + hardware entropy sources

### src/rest.cpp / .h
- REST API endpoint handlers for `/rest/block/`, `/rest/tx/`, `/rest/headers/`, `/rest/blockhashbyheight/`, `/rest/chaininfo/`, `/rest/mempool/`, `/rest/getutxos/`, `/rest/blockfilter/`
- `ParseDataFormat()` → parses requested response format (JSON, binary, hex) from URL extension

### src/scheduler.cpp / .h
- `CScheduler` (class) → priority-queue-based task scheduler running callbacks on a background thread
  - `scheduleEvery()` → runs a function repeatedly at a fixed interval
  - `scheduleFromNow()` → runs a function once after a delay
  - `schedule()` → schedules a function at a specific time point
  - `MockForward()` → advances scheduler clock for testing
- `SingleThreadedSchedulerClient` (class) → ensures callbacks execute serially even with concurrent scheduling

### src/serialize.h
- `Serialize()` / `Unserialize()` → template framework for binary serialization of all Bitcoin/DigiByte data types
- `CSizeComputer` (class) → dry-run serializer that computes serialized size without writing data
- `VarIntFormatter` / `CompactSizeFormatter` — variable-length integer encoding formats
- Serialization wrappers: `VARINT()`, `COMPACTSIZE()`, `LIMITED_STRING()`, `FLATDATA()`

### src/shutdown.cpp / .h
- `StartShutdown()` → signals the node to begin graceful shutdown
- `AbortShutdown()` → cancels a pending shutdown request
- `ShutdownRequested()` → returns true if shutdown has been signaled
- `WaitForShutdown()` → blocks calling thread until shutdown completes

### src/signet.cpp / .h
- `CheckSignetBlockSolution()` → validates block is signed by authorized signet signer keys
- `SignetTxs` (class) → extracts signet commitment and challenge from coinbase transaction

### src/streams.cpp / .h
- `DataStream` / `CDataStream` (class) → in-memory byte stream for serialization/deserialization of Bitcoin objects
- `AutoFile` / `CAutoFile` (class) → RAII file wrapper with automatic serialization support and optional XOR obfuscation
- `BufferedFile` (class) → file reader with read-ahead buffering and rewind capability (for block file scanning)
- `SpanReader` (class) → reads serialized data from a Span without copying
- `CVectorWriter` (class) → writes serialized data directly into a vector
- `OverrideStream` (class) → wraps a stream with overridden version/type for serialization format control

### src/sync.cpp / .h
- Lock debugging infrastructure for detecting potential deadlocks in the multi-threaded codebase
- `RecursiveMutex` / `Mutex` → mutex types with optional deadlock detection in debug builds
- `LOCK()` / `LOCK2()` → macros for acquiring locks with debug tracking
- `AssertLockHeld()` → compile-time + runtime assertion that a lock is held
- `LockOrdering` → enforces a strict global lock ordering to prevent deadlocks

### src/timedata.cpp / .h
- `CMedianFilter` (class) → rolling median filter for network time adjustment
- `GetTimeOffset()` → returns median offset between local clock and peer-reported times
- `GetAdjustedTime()` → returns current time adjusted by median peer offset
- `AddTimeData()` → incorporates a peer's reported timestamp into the median filter

### src/torcontrol.cpp / .h
- `TorController` (class) → manages Tor control port connection for automatic hidden service creation
  - Creates/destroys .onion hidden service for incoming P2P connections
- `TorControlConnection` (class) → low-level async Tor control protocol implementation
- `StartTorControl()` / `InterruptTorControl()` / `StopTorControl()` → lifecycle management
- `DefaultOnionServiceTarget()` → returns the default local address:port for onion service

### src/txdb.cpp / .h
- `CCoinsViewDB` (class extends CCoinsView) → LevelDB-backed UTXO set storage
  - `GetCoin()` → reads a UTXO from the database
  - `HaveCoin()` → checks UTXO existence without full deserialization
  - `BatchWrite()` → writes a batch of UTXO changes to LevelDB
  - `GetBestBlock()` → returns the block hash this UTXO set represents
  - `Cursor()` → creates an iterator over all UTXOs (for UTXO set hash computation)

### src/txmempool.cpp / .h
- `CTxMemPool` (class) → in-memory pool of unconfirmed transactions awaiting inclusion in a block
  - `addUnchecked()` → adds a transaction entry to the mempool (after validation)
  - `removeRecursive()` → removes a transaction and all descendants from the mempool
  - `removeForBlock()` → removes transactions included in a newly connected block
  - `check()` → performs internal consistency checks on mempool data structures
  - `TrimToSize()` → evicts lowest-feerate transactions to enforce mempool size limit
  - `GetTransactionAncestry()` → returns ancestor count/size for package relay validation
  - `CalculateDescendants()` → computes all descendant transactions of a given entry
  - `GetMinFee()` → returns minimum fee rate to enter the mempool (dynamic, based on fullness)
  - `info()` → returns mempool entry info for a specific transaction
  - `exists()` → checks if a transaction is in the mempool
  - `get()` → retrieves a transaction reference by hash
  - `size()` → returns transaction count in mempool
- `CCoinsViewMemPool` (class) → layered view that overlays mempool UTXOs on top of the chain UTXO set
- `TestLockPointValidity()` → checks if a transaction's lockpoint is still valid after chain reorganization
- `CTxMemPoolEntry` → see `src/kernel/mempool_entry.h`

### src/txorphanage.cpp / .h
- `TxOrphanage` (class) → manages orphan transactions (those with missing parent inputs)
  - `AddTx()` → adds an orphan transaction, limited per-peer
  - `EraseTx()` → removes an orphan by hash
  - `EraseForPeer()` → removes all orphans from a disconnected peer
  - `GetTxToReconsider()` → returns an orphan to revalidate after its parent arrives
  - `HaveTx()` → checks if an orphan exists
  - `LimitOrphans()` → enforces maximum orphanage size by random eviction

### src/txrequest.cpp / .h
- `TxRequestTracker` (class) → tracks in-flight transaction download requests across peers with priority, timeouts, and deduplication
  - `ReceivedInv()` → records that a peer announced a transaction
  - `RequestedTx()` → marks a transaction as requested from a peer
  - `ReceivedResponse()` → records that a response (tx or NOTFOUND) was received
  - `GetRequestable()` → returns transactions eligible for requesting from a given peer

### src/uint256.cpp / .h
- `base_blob<BITS>` (class template) → fixed-size opaque byte array (base for hash types)
- `uint160` (class) → 160-bit hash (RIPEMD160/Hash160 output)
- `uint256` (class) → 256-bit hash (SHA256d/block hash/txid)
- `uint512` (class) → 512-bit hash (used by multi-hash mining algorithms)
- `uint256S()` → constructs uint256 from hex string

### src/undo.h
- `CTxUndo` (class) → undo data for a single transaction: vector of Coins consumed by inputs (for disconnect/reorg)
- `CBlockUndo` (class) → undo data for an entire block: all CTxUndo entries (excluding coinbase)

### src/validation.cpp / .h
- ⚠️ Contains DigiDollar activation height checks, oracle price lookups, DD transaction validation
- `Chainstate` (class) → manages a single validated chain state (UTXO set + block index)
  - `ActivateBestChain()` → selects and activates the best valid chain tip, connecting new blocks
  - `ConnectTip()` → connects a single new block to the chain tip, executing all transactions
  - `DisconnectTip()` → disconnects the current tip (for reorg), restoring UTXOs from undo data
  - `DisconnectBlock()` → undoes all transactions in a block, restoring previous UTXO state
  - `InvalidateBlock()` → marks a block and its descendants as invalid (manual override)
  - `PreciousBlock()` → hints the node to prefer a specific valid block tip
  - `ResetBlockFailureFlags()` → clears failure flags from a previously-invalidated block
  - `LoadChainTip()` → loads the chain tip from disk on startup
  - `FlushStateToDisk()` → persists UTXO cache and block index to disk
  - `GetCoinsCacheSizeState()` → returns whether UTXO cache is within limits or needs flushing
  - `InvalidChainFound()` → logs when a chain with more work than current tip is found to be invalid
  - `CheckForkWarningConditions()` → warns if a valid fork with significant work exists
- `ChainstateManager` (class) → manages one or two Chainstate objects (main + optional snapshot)
  - `ProcessNewBlock()` → validates and stores a new block, activates best chain if it extends the tip
  - `ProcessNewBlockHeaders()` → validates a batch of new block headers for header-first sync
  - `AcceptBlock()` → validates block against contextual rules and writes to disk
  - `AcceptBlockHeader()` → validates and indexes a new block header
  - `ProcessTransaction()` → validates and submits a transaction to the mempool
  - `IsInitialBlockDownload()` → returns true if node is still catching up to the network tip
  - `ActiveChainstate()` / `ActiveChain()` / `ActiveTip()` → access the current active chain
  - `GenerateCoinbaseCommitment()` → creates SegWit witness commitment for coinbase
  - `SnapshotBlockhash()` → returns the snapshot base block if using assumeUTXO
- `CheckBlock()` → validates block structure: size limits, merkle root, duplicate txns, first tx is coinbase, algo-specific PoW
- `CheckFinalTxAtTip()` → checks transaction finality (locktime) against current chain tip
- `HasValidProofOfWork()` → validates PoW for a vector of block headers
- `IsBlockMutated()` → detects witness malleation attacks on block data
- `CalculateHeadersWork()` → sums proof-of-work across a vector of headers
- ⚠️ `GetOraclePriceForTransaction()` → retrieves oracle-reported DGB/USD price for DD transaction validation
- `GetBlockSubsidy()` → calculates mining reward for a given block height (halving schedule)
- `IsAlgoActive()` → checks if a specific mining algorithm is active at a given chain position
- `CVerifyDB` (class) → verifies blockchain database integrity on startup
- `CScriptCheck` (class) → deferred script verification task for parallel validation
- `StartScriptCheckWorkerThreads()` / `StopScriptCheckWorkerThreads()` → manages parallel script checker thread pool
- `MemPoolAccept` (class, internal) → orchestrates mempool transaction acceptance: PreChecks, PolicyScriptChecks, ConsensusScriptChecks, Finalize
- `UpdateCoins()` → applies transaction's input spends and output creations to the UTXO set
- `GuessVerificationProgress()` → estimates sync progress as fraction based on timestamps
- `PruneBlockFilesManual()` → manually prunes block files up to a specified height

### src/validationinterface.cpp / .h
- `CValidationInterface` (class) → abstract observer interface for blockchain events
  - `UpdatedBlockTip()` → called when the active chain tip changes
  - `TransactionAddedToMempool()` → called when a transaction enters the mempool
  - `TransactionRemovedFromMempool()` → called when a transaction is evicted/confirmed/conflicted out
  - `BlockConnected()` / `BlockDisconnected()` → called when blocks are connected/disconnected
  - `ChainStateFlushed()` → called after UTXO set is flushed to disk
- `CMainSignals` (class) → signal dispatcher that broadcasts validation events to all registered interfaces
- `RegisterValidationInterface()` / `UnregisterValidationInterface()` → registers/unregisters an observer
- `SyncWithValidationInterfaceQueue()` → blocks until all queued validation callbacks have been processed

### src/version.h
- `PROTOCOL_VERSION` → current P2P protocol version (70019)
- Protocol version constants for feature negotiation (SHORT_IDS_BLOCKS_VERSION, etc.)

### src/versionbits.cpp / .h
- `AbstractThresholdConditionChecker` (class) → BIP9-style soft fork activation state machine
  - `GetStateFor()` → returns activation state (DEFINED, STARTED, LOCKED_IN, ACTIVE, FAILED) for a deployment
  - `GetStateSinceHeightFor()` → returns the block height where current state began
- `VersionBitsCache` (class) → caches BIP9 deployment states to avoid recomputation
  - `StateSinceHeight()` → cached version of state query
  - `Clear()` → invalidates cache (after reorg)
- `ThresholdState` enum → DEFINED, STARTED, LOCKED_IN, ACTIVE, FAILED

### src/warnings.cpp / .h
- `SetMiscWarning()` → sets a global warning message displayed in RPC and GUI
- `SetfLargeWorkInvalidChainFound()` → flags that a high-work invalid chain was detected
- `GetWarnings()` → returns current warning messages (pre-release, large fork, etc.)

---

## Source Files — src/bench/

### src/bench/bench.cpp / .h
- `Bench` (class) → nanobench-based micro-benchmarking framework wrapper
- `BenchRunner` → registers and runs all benchmarks

### src/bench/bench_digibyte.cpp
- Entry point for the benchmark binary (`bench_digibyte`)

### Key benchmarks:
- `bench/addrman.cpp` → AddrMan Add/Select/GetAddr performance
- `bench/base58.cpp` → Base58 encode/decode speed
- `bench/bech32.cpp` → Bech32 encode/decode speed
- `bench/bip324_ecdh.cpp` → BIP324 ECDH key exchange performance
- `bench/block_assemble.cpp` → block template assembly with mempool
- `bench/ccoins_caching.cpp` → UTXO cache access patterns
- `bench/chacha20.cpp` → ChaCha20 cipher throughput
- `bench/checkblock.cpp` → `CheckBlock()` validation performance
- `bench/checkqueue.cpp` → parallel script check queue throughput
- `bench/coin_selection.cpp` → wallet coin selection algorithms
- `bench/crypto_hash.cpp` → all hash functions (SHA256, RIPEMD160, SipHash, MurmurHash3, multi-algo)
- `bench/descriptors.cpp` → descriptor parsing and expansion
- `bench/disconnected_transactions.cpp` → disconnected tx pool during reorg
- `bench/duplicate_inputs.cpp` → duplicate input detection
- `bench/ellswift.cpp` → ElligatorSwift encoding (BIP324)
- `bench/gcs_filter.cpp` → Golomb-coded set filter match performance
- `bench/hashpadding.cpp` → SHA256 padding overhead
- `bench/load_external.cpp` → external block loading
- `bench/lockedpool.cpp` → secure memory allocator performance
- `bench/logging.cpp` → logging overhead
- `bench/mempool_eviction.cpp` → mempool eviction under pressure
- `bench/mempool_stress.cpp` → mempool under high transaction volume
- `bench/merkle_root.cpp` → Merkle root computation
- `bench/oracle_performance.cpp` → ⚠️ oracle message validation and bundle processing performance
- `bench/peer_eviction.cpp` → peer eviction candidate selection
- `bench/poly1305.cpp` → Poly1305 MAC throughput
- `bench/pool.cpp` → PoolAllocator performance
- `bench/prevector.cpp` → prevector small-buffer optimization
- `bench/rollingbloom.cpp` → rolling bloom filter
- `bench/rpc_blockchain.cpp` → RPC blockchain query performance
- `bench/rpc_mempool.cpp` → RPC mempool query performance
- `bench/streams_findbyte.cpp` → stream byte search
- `bench/strencodings.cpp` → hex/base encoding
- `bench/util_time.cpp` → time utility functions
- `bench/verify_script.cpp` → script verification (P2PKH, P2WPKH, P2WSH, P2TR)
- `bench/wallet_balance.cpp` → wallet balance calculation
- `bench/wallet_create_tx.cpp` → transaction creation performance
- `bench/wallet_loading.cpp` → wallet database loading
- `bench/xor.cpp` → XOR obfuscation performance

---

## Source Files — src/common/

### src/common/args.cpp / .h
- `ArgsManager` (class) → parses and manages command-line arguments, config file settings, and network-specific sections
  - `ParseParameters()` → parses argc/argv into internal settings map
  - `ReadConfigFiles()` → reads and parses digibyte.conf with section support ([main], [test], [regtest])
  - `GetArg()` / `GetBoolArg()` / `GetIntArg()` → retrieves typed setting values with defaults
  - `IsArgSet()` → checks if an argument was provided
  - `SoftSetArg()` / `SoftSetBoolArg()` → sets a default that can be overridden
  - `GetDataDirNet()` / `GetDataDirBase()` → returns data directory path (network-specific or base)
  - `GetChainType()` → returns which chain (mainnet/testnet/signet/regtest) is configured
- `HelpRequested()` → checks if -help/-? was passed
- `SetupHelpOptions()` → registers -help and -version arguments

### src/common/bloom.cpp / .h
- `CBloomFilter` (class) → BIP 37 bloom filter for SPV clients to filter relevant transactions
  - `insert()` → adds a data element to the filter
  - `contains()` → tests membership (probabilistic, false positives possible)
  - `IsRelevantAndUpdate()` → tests if a transaction matches the filter and auto-updates with matched outpoints
  - `IsWithinSizeConstraints()` → validates filter size is within protocol limits
- `CRollingBloomFilter` (class) → space-efficient rolling bloom filter with automatic expiration of old entries

### src/common/config.cpp
- `ArgsManager::ReadConfigStream()` → parses a config file stream into settings
- `ArgsManager::ReadConfigFiles()` → reads main config file + all includeconf files
- `AbsPathForConfigVal()` → resolves relative paths in config to absolute paths

### src/common/init.cpp / .h
- `EnsureDataDir()` → creates data directory if it doesn't exist
- `SanityChecks()` → performs basic sanity checks (e.g., verify crypto functions)
- `SetGlobals()` → initializes global logging, args, etc.
- `UnsetGlobals()` → cleans up globals on shutdown

### src/common/interfaces.cpp
- `MakeEcho()` → factory for IPC echo interface (testing)

### src/common/run_command.cpp / .h
- `RunCommandParseJSON()` → executes external command and parses stdout as JSON (for external signers)

### src/common/settings.cpp / .h
- `ReadSettings()` → reads persistent settings from `settings.json`
- `WriteSettings()` → writes persistent settings to `settings.json`
- `GetSetting()` → retrieves a setting value with priority: forced > command-line > RW settings > config file
- `OnlyHasDefaultSectionSetting()` → checks if a setting only appears in the default config section

### src/common/system.cpp / .h
- `SetupEnvironment()` → sets up locale, UTF-8 environment for cross-platform compatibility
- `SetupNetworking()` → initializes platform networking (Winsock on Windows)
- `GetNumCores()` → returns number of CPU cores for thread pool sizing
- `runCommand()` → executes a shell command (for `-alertnotify`, `-blocknotify`)
- `ShellEscape()` → escapes a string for safe shell command usage

### src/common/url.cpp / .h
- `urlDecode()` → URL-decodes a percent-encoded string

---

## Source Files — src/compat/

### src/compat/assumptions.h
- Static assertions verifying platform assumptions (2's complement, byte sizes, integer widths)

### src/compat/compat.h
- Cross-platform compatibility definitions: socket types, error codes, `MAX_PATH`, `closesocket()`

### src/compat/cpuid.h
- `GetCPUID()` → wrapper around x86 CPUID instruction for detecting hardware crypto (SHA-NI, SSE4, AVX2)

### src/compat/endian.h / byteswap.h
- `htole16/32/64()`, `le16/32/64toh()` → host-to-little-endian and reverse byte order conversions
- `bswap_16/32/64()` → byte swap functions (platform-specific fast implementations)

### src/compat/glibc_compat.cpp / glibc_sanity.cpp / glibcxx_sanity.cpp
- Compatibility shims for older glibc versions and sanity checks for C/C++ standard library

### src/compat/stdin.cpp / .h
- `SetStdinEcho()` → enables/disables stdin echo (for password input)
- `StdinReady()` → checks if stdin has data available (non-blocking)

---

## Source Files — src/consensus/

### src/consensus/amount.h
- `CAmount` (typedef int64_t) → monetary amount in satoshis (1 DGB = 100,000,000 satoshis)
- `MAX_MONEY` → 21 billion DGB maximum supply cap
- `MoneyRange()` → validates an amount is within [0, MAX_MONEY]

### src/consensus/consensus.h
- `MAX_BLOCK_SERIALIZED_SIZE` → 4MB maximum serialized block size
- `MAX_BLOCK_WEIGHT` → 4M weight units maximum block weight
- `MAX_BLOCK_SIGOPS_COST` → maximum signature operations per block (80,000)
- `WITNESS_SCALE_FACTOR` → witness discount factor (4x)
- `COINBASE_MATURITY` → blocks before coinbase outputs can be spent (100)

### src/consensus/merkle.cpp / .h
- `ComputeMerkleRoot()` → builds Merkle tree from transaction hashes, returns root hash
- `BlockMerkleRoot()` → computes Merkle root of all transactions in a block
- `BlockWitnessMerkleRoot()` → computes witness Merkle root (includes witness data in hash)

### src/consensus/params.h
- `Consensus::Params` (struct) → all consensus parameters for a chain: genesis hash, subsidy halving interval, BIP activation heights, PoW limits per algo, difficulty adjustment heights, MultiShield parameters
  - `hashGenesisBlock` → genesis block hash
  - `nSubsidyHalvingInterval` → blocks between halvings
  - `powLimit` / `powLimitSHA` / `powLimitScrypt` / `powLimitGroestl` / `powLimitSkein` / `powLimitQubit` → per-algo difficulty limits
  - `nMultiAlgoStartBlock` → height where 5-algo mining begins
  - `nDigiShieldStartBlock` → height where DigiShield difficulty adjustment activates
  - `nOdoStartBlock` / `nOdoEndBlock` → height range for Odocrypt algorithm (replaces Qubit)
  - ⚠️ `nDDActivationHeight` / `nOracleActivationHeight` / `nDigiDollarPhase2Height` → DigiDollar activation heights
  - ⚠️ `nDDOracleEpochBlocks` / `nOracleEpochLength` / `nOracleRequiredMessages` → oracle system parameters
  - ⚠️ `vOraclePublicKeys` → hardcoded oracle signing keys
- `BuriedDeployment` enum → activation heights for BIP34, BIP65, BIP66, CSV, SegWit, Taproot, Odocrypt
- ⚠️ `IsOracleActive()` → checks if oracle system is active at a given height

### src/consensus/tx_check.cpp / .h
- `CheckTransaction()` → validates transaction structure: non-empty inputs/outputs, output amounts positive and within range, no duplicate inputs, coinbase scriptSig size limits

### src/consensus/tx_verify.cpp / .h
- `IsFinalTx()` → checks transaction finality based on nLockTime and nSequence
- `GetLegacySigOpCount()` → counts signature operations in a transaction's scripts (pre-P2SH)
- `GetP2SHSigOpCount()` → counts sigops in P2SH redeem scripts (after BIP16)
- `GetTransactionSigOpCost()` → calculates weighted sigop cost including SegWit discount
- `CalculateSequenceLocks()` → computes BIP68 relative timelock heights/times for all inputs
- `EvaluateSequenceLocks()` → checks if sequence lock conditions are satisfied at a given block
- `SequenceLocks()` → combined sequence lock check for mempool admission

### src/consensus/validation.h
- `TxValidationResult` enum → transaction rejection reasons (CONSENSUS, RECENT_CONSENSUS_CHANGE, TX_NOT_STANDARD, TX_MISSING_INPUTS, TX_MEMPOOL_POLICY, etc.)
- `BlockValidationResult` enum → block rejection reasons (CONSENSUS, BLOCK_CACHED_INVALID, BLOCK_HEADER_LOW_WORK, etc.)
- `ValidationState<T>` (class template) → carries validation result, rejection reason, and debug message
- `TxValidationState` / `BlockValidationState` → concrete validation state classes
- `GetTransactionWeight()` → calculates transaction weight (base_size * 3 + total_size)
- `GetBlockWeight()` → calculates total block weight
- `GetWitnessCommitmentIndex()` → finds the SegWit commitment output in coinbase transaction

### src/consensus/dca.cpp / .h
- ⚠️ `DynamicCollateralAdjustment` (class) → DigiDollar DCA consensus logic — calculates required DGB collateral for minting DigiDollars based on volatility-adjusted ratios
  - `CalculateRequiredCollateral()` → computes DGB collateral needed for a DD mint at current oracle price
  - `ValidateCollateral()` → validates that a DD mint transaction has sufficient collateral
  - `GetTierForAmount()` → determines which of the 4 collateral tiers applies to a mint amount

### src/consensus/err.cpp / .h
- ⚠️ `EmergencyRedemptionRatio` (class) → DigiDollar ERR consensus logic — manages emergency redemption when DGB/USD price drops significantly
  - `CalculateRedemptionAmount()` → computes DGB returned when redeeming DigiDollars under ERR conditions
  - `ValidateRedemption()` → validates a DD redemption transaction against ERR rules
  - `IsEmergencyActive()` → checks if emergency redemption mode is triggered based on price deviation

### src/consensus/volatility.cpp / .h
- ⚠️ `VolatilityMonitor` (class) → tracks DGB/USD price volatility from oracle data for collateral tier adjustments
  - `AddPricePoint()` → records a new oracle price observation
  - `GetCurrentVolatility()` → calculates current annualized volatility from recent price history
  - `GetVolatilityTier()` → maps current volatility to a collateral multiplier tier
  - `IsHighVolatility()` → checks if volatility exceeds the high-risk threshold
- `FormatVolatility()` → formats volatility as human-readable percentage string
- `CalculatePercentageChange()` → computes percentage change between two prices
- `ExceedsThreshold()` → checks if price change exceeds a given percentage threshold

---

## Source Files — src/crypto/

### Mining Algorithm Hashes (5 DigiByte Algorithms)

#### src/crypto/sha256.cpp / .h — **SHA-256d** (Algorithm 0)
- `CSHA256` (class) → SHA-256 hasher with hardware acceleration detection (SSE4, AVX2, SHA-NI, ARM-SHANI)
  - `Write()` → feeds data into the hash
  - `Finalize()` → produces 32-byte hash output
  - `Reset()` → resets hasher state for reuse
- `SHA256AutoDetect()` → detects CPU capabilities and selects fastest SHA-256 implementation
- `SHA256D64()` → optimized double-SHA256 for 64-byte inputs (Merkle tree inner nodes)
- Hardware-accelerated implementations: `sha256_sse4.cpp`, `sha256_sse41.cpp`, `sha256_avx2.cpp`, `sha256_x86_shani.cpp`, `sha256_arm_shani.cpp`
- `sha256_Y.cpp / .h` → SHA-256 variant used in multi-algo proof-of-work context

#### src/crypto/scrypt.cpp / .h — **Scrypt** (Algorithm 1)
- `scrypt_1024_1_1_256()` → Scrypt hash with N=1024, r=1, p=1 parameters (Litecoin-compatible, memory-hard)
- `scrypt_1024_1_1_256_sp_generic()` → generic C implementation with explicit scratchpad
- `scrypt_1024_1_1_256_sp_sse2()` → SSE2-optimized Scrypt implementation
- `scrypt_detect_sse2()` → runtime detection of SSE2 support for Scrypt acceleration

#### src/crypto/hashgroestl.h + groestl.cpp — **Groestl** (Algorithm 2)
- `HashGroestl()` → computes Groestl-512 hash truncated to 256 bits (one of 5 DigiByte mining algorithms)
- `sph_groestl512_init/update/close()` → low-level Groestl-512 sponge functions

#### src/crypto/hashskein.h + skein.cpp — **Skein** (Algorithm 3)
- `HashSkein()` → computes Skein-512-256 hash (SHA-3 finalist, one of 5 DigiByte mining algorithms)
- `sph_skein512_init/update/close()` → low-level Skein-512 functions

#### src/crypto/hashqubit.h + related — **Qubit** (Algorithm 4, replaced by Odocrypt at block 9,112,320)
- `HashQubit()` → computes Qubit hash (chained Luffa→CubeHash→SHAvite→SIMD→ECHO, one of 5 DigiByte mining algorithms)
- Component hash functions: `luffa.cpp`, `cubehash.cpp`, `shavite.cpp`, `simd.cpp`, `echo.cpp`
- Additional Qubit components: `blake.cpp`, `bmw.cpp`, `jh.cpp`, `keccak.cpp`

#### src/crypto/odocrypt.cpp / .h + hashodo.h — **Odocrypt** (replaces Qubit after block 9,112,320)
- `OdoCrypt` (class) → FPGA/ASIC-resistant cipher that changes its algorithm every 10 days based on a time-derived key
  - `Encrypt()` → encrypts data using the current Odocrypt configuration
- `HashOdo()` → computes Odocrypt hash with time-rotating key (DigiByte's 5th mining algorithm post-Odo activation)
- `OdoKey()` → derives the Odocrypt key from block timestamp and consensus params

### General Cryptographic Primitives

#### src/crypto/aes.cpp / .h
- `AES256Encrypt` / `AES256Decrypt` (classes) → AES-256 ECB mode encryption/decryption
- `AES256CBCEncrypt` / `AES256CBCDecrypt` (classes) → AES-256 CBC mode with PKCS#7 padding (wallet encryption)

#### src/crypto/chacha20.cpp / .h
- `ChaCha20Aligned` (class) → ChaCha20 stream cipher (aligned blocks only)
- `ChaCha20` (class) → ChaCha20 with arbitrary-length input handling
- `FSChaCha20` (class) → forward-secure ChaCha20 that re-keys after every message (BIP324)

#### src/crypto/chacha20poly1305.cpp / .h
- `AEADChaCha20Poly1305` (class) → AEAD authenticated encryption for BIP324 P2P messages
  - `Encrypt()` → encrypts and authenticates a message
  - `Decrypt()` → decrypts and verifies authentication tag
- `FSChaCha20Poly1305` (class) → forward-secure AEAD with automatic rekeying

#### src/crypto/hkdf_sha256_32.cpp / .h
- `CHKDF_HMAC_SHA256_L32` (class) → HKDF key derivation (extract + expand) producing 32-byte output

#### src/crypto/hmac_sha256.cpp / .h
- `CHMAC_SHA256` (class) → HMAC-SHA256 message authentication code

#### src/crypto/hmac_sha512.cpp / .h
- `CHMAC_SHA512` (class) → HMAC-SHA512 for BIP32 key derivation

#### src/crypto/muhash.cpp / .h
- `Num3072` (class) → 3072-bit number arithmetic for MuHash
- `MuHash3072` (class) → multiplicative hash set for efficient UTXO set hash (O(1) insert/remove)
  - `Insert()` / `Remove()` → adds/removes elements from the set hash
  - `Finalize()` → produces final 256-bit hash of the set

#### src/crypto/poly1305.cpp / .h
- `Poly1305` (class) → Poly1305 one-time authenticator (MAC)

#### src/crypto/ripemd160.cpp / .h
- `CRIPEMD160` (class) → RIPEMD-160 hash (used in combination with SHA-256 for address generation)

#### src/crypto/sha1.cpp / .h
- `CSHA1` (class) → SHA-1 hash (used only for P2P message checksum in legacy transport)

#### src/crypto/sha3.cpp / .h
- `SHA3_256` (class) → SHA-3 (Keccak-256) hash
- `KeccakF()` → Keccak-f[1600] permutation function

#### src/crypto/sha512.cpp / .h
- `CSHA512` (class) → SHA-512 hash (used in HMAC-SHA512 for BIP32)

#### src/crypto/siphash.cpp / .h
- `CSipHasher` (class) → SipHash-2-4 for hash table randomization (DoS-resistant)
- `SipHashUint256()` → SipHash of a uint256 (for tx/block hash table lookups)

#### src/crypto/sph_*.h
- SPH (Sphlib) header files providing portable hash function interfaces for all multi-algo mining components

---

## Source Files — src/index/

### src/index/base.cpp / .h
- `BaseIndex` (class) → abstract base class for blockchain indexing with background sync, reorg handling, and persistence
  - `Init()` → initializes the index and starts background sync from last indexed block
  - `BlockConnected()` / `BlockDisconnected()` → processes new/reverted blocks
  - `Rewind()` → handles chain reorganization by rewinding the index
  - `Start()` → begins background synchronization thread
  - `Stop()` → stops the index and commits final state
  - `GetSummary()` → returns sync progress information
- `BaseIndex::DB` (class) → LevelDB wrapper for index storage with best-block tracking

### src/index/blockfilterindex.cpp / .h
- `BlockFilterIndex` (class extends BaseIndex) → BIP 157/158 compact block filter index
  - `LookupFilter()` → retrieves a block filter by block hash
  - `LookupFilterHeader()` → retrieves a filter header for a block
  - `LookupFilterRange()` → retrieves a range of consecutive block filters
- `GetBlockFilterIndex()` → returns the index instance for a filter type
- `InitBlockFilterIndex()` → creates and initializes a block filter index
- `ForEachBlockFilterIndex()` → iterates over all active filter indexes

### src/index/coinstatsindex.cpp / .h
- `CoinStatsIndex` (class extends BaseIndex) → maintains running UTXO set hash (MuHash) per block
  - `LookupStats()` → retrieves UTXO set statistics (hash, total amount, tx count) at a given block

### src/index/txindex.cpp / .h
- `TxIndex` (class extends BaseIndex) → transaction-to-block-position index for `getrawtransaction` RPC
  - `FindTx()` → looks up a transaction's disk position by txid

### src/index/disktxpos.h
- `CDiskTxPos` (struct) → on-disk position of a transaction: block file position + offset within block

---

## Source Files — src/init/

### src/init/common.cpp / .h
- `init::EnsureDataDir()` → creates the data directory with proper permissions
- `init::SetGlobals()` → initializes global state (logging, args)
- `init::UnsetGlobals()` → cleans up global state
- `init::SanityChecks()` → runs startup sanity checks (glibcxx, crypto)

### src/init/digibyted.cpp
- `interfaces::MakeNodeInit()` → factory for daemon-mode node initialization

### src/init/digibyte-gui.cpp
- `interfaces::MakeGuiInit()` → factory for GUI-mode node initialization

### src/init/digibyte-node.cpp
- `interfaces::MakeNodeInit()` → factory for multiprocess node initialization (Bitcoin Core IPC)

### src/init/digibyte-qt.cpp
- `interfaces::MakeGuiInit()` → factory for Qt GUI initialization (alias)

### src/init/digibyte-wallet.cpp
- `interfaces::MakeWalletInit()` → factory for wallet-tool-only initialization

---

## Source Files — src/interfaces/

### src/interfaces/chain.h
- `interfaces::Chain` (class) → abstract interface that wallet and other clients use to access blockchain state
  - `getHeight()` → returns current chain height
  - `getBlockHash()` → returns block hash at a given height
  - `findBlock()` → locates a block by hash with optional data retrieval
  - `findAncestorByHeight()` → finds an ancestor block at a specific height
  - `estimateSmartFee()` → estimates fee rate for confirmation within N blocks
  - `mempool()` → access to mempool for UTXO lookups
  - `broadcastTransaction()` → submits transaction to the network
  - `requestMempoolTransactions()` → loads all mempool transactions into a notification sink
- `interfaces::FoundBlock` (class) → builder pattern for specifying which block data to retrieve
- `interfaces::Chain::Notifications` (class) → callback interface for chain events (tip change, tx added/removed)

### src/interfaces/handler.h
- `interfaces::Handler` (class) → RAII wrapper for signal connections, auto-disconnects on destruction
- `MakeSignalHandler()` → creates handler from a Boost.Signals2 connection
- `MakeCleanupHandler()` → creates handler that runs cleanup function on destruction

### src/interfaces/init.h
- `interfaces::Init` (class) → abstract initialization interface for multiprocess architecture
  - `makeNode()` → creates a Node interface
  - `makeChain()` → creates a Chain interface
  - `makeWalletLoader()` → creates a WalletLoader interface
  - `makeEcho()` → creates an Echo interface (for IPC testing)

### src/interfaces/ipc.h
- `interfaces::Ipc` (class) → inter-process communication interface for multiprocess Bitcoin node architecture

### src/interfaces/node.h
- `interfaces::Node` (class) → abstract interface for controlling the node from GUI/RPC
  - `initLogging()` / `initParameterInteraction()` → initialization steps
  - `startShutdown()` / `shutdownRequested()` → shutdown control
  - `getNodeCount()` → peer count
  - `getNodesStats()` → per-peer statistics
  - `getTotalBytesRecv()` / `getTotalBytesSent()` → bandwidth counters
  - `getMempoolSize()` / `getMempoolDynamicUsage()` → mempool stats
  - `getHeaderTip()` / `getNumBlocks()` → chain sync status
  - `isInitialBlockDownload()` → IBD status check
  - `getReindex()` → reindex progress
- `interfaces::ExternalSigner` (class) → interface for hardware wallet operations

### src/interfaces/wallet.h
- `interfaces::Wallet` (class) → abstract wallet interface for GUI and RPC
  - `encryptWallet()` / `lock()` / `unlock()` / `changeWalletPassphrase()` → encryption operations
  - `getBalance()` → returns wallet balance breakdown (confirmed, unconfirmed, immature)
  - `getCoins()` → returns available UTXOs
  - `createTransaction()` → builds and signs a transaction
  - `commitTransaction()` → broadcasts a signed transaction
  - `getAddresses()` → returns all wallet addresses with labels
  - `signMessage()` → signs a message with a wallet key
  - `backupWallet()` → creates wallet backup file
  - ⚠️ `getDigiDollarWallet()` → returns DigiDollar wallet interface pointer
- `interfaces::WalletLoader` (class extends ChainClient) → loads/creates/lists wallets
- `MakeWallet()` → creates Wallet interface from CWallet
- `MakeWalletLoader()` → creates WalletLoader interface

---

## Source Files — src/ipc/

### src/ipc/interfaces.cpp
- `MakeIpc()` → factory for IPC implementation (multiprocess node architecture)

### src/ipc/process.h
- `ipc::Process` (class) → manages child processes for multiprocess architecture
  - `spawn()` → spawns a new node subprocess
  - `waitSpawned()` → waits for subprocess to be ready
- `MakeProcess()` → factory for Process implementation

### src/ipc/protocol.h
- `ipc::Protocol` (class) → Cap'n Proto-based IPC protocol for type-safe cross-process communication

### src/ipc/context.h
- `ipc::Context` (struct) → shared context passed through IPC connections

---

## Source Files — src/kernel/

### src/kernel/chain.cpp / .h
- `MakeBlockInfo()` → creates BlockInfo struct from CBlockIndex for kernel interface
- `ChainstateRole` enum → NORMAL or BACKGROUND (for assumeUTXO snapshot validation)

### src/kernel/chainparams.cpp / .h
- `CChainParams` (class) → full chain parameters: network magic bytes, default port, genesis block, seeds, checkpoints, consensus params, address prefixes
  - `Main()` → creates mainnet parameters (port 12024, genesis Jan 10 2014, 5-algo PoW, DigiShield/MultiShield activation heights)
  - `TestNet()` → creates testnet parameters (port 12026, relaxed difficulty)
  - `SigNet()` → creates signet parameters (signed block test network)
  - `RegTest()` → creates regtest parameters (instant mining, no real PoW)
  - ⚠️ `GetOracleNode()` → looks up oracle node info by ID from hardcoded oracle configuration
  - ⚠️ `GetActiveOracleCount()` → returns number of active oracle nodes in current chain params
  - Contains all DigiByte-specific multi-algo activation heights, Odocrypt parameters, and ⚠️ DigiDollar/Oracle activation heights

### src/kernel/chainstatemanager_opts.h
- `ChainstateManagerOpts` (struct) → configuration options for ChainstateManager (worker threads, assumed-valid block, etc.)

### src/kernel/blockmanager_opts.h
- `BlockManagerOpts` (struct) → configuration for block storage (prune target, fast prune flag)

### src/kernel/checks.cpp / .h
- `SanityChecks()` → kernel-level sanity checks (ECC, random number generator)

### src/kernel/coinstats.cpp / .h
- `ComputeUTXOStats()` → computes full UTXO set statistics (hash, total coins, total amount) by scanning entire UTXO database
- `ApplyCoinHash()` / `RemoveCoinHash()` → incrementally updates MuHash when a UTXO is added/removed
- `GetBogoSize()` → estimates in-memory size of a UTXO entry

### src/kernel/context.cpp / .h
- `kernel::Context` (struct) → minimal kernel context for library-mode usage (ECC init, sanity checks)

### src/kernel/cs_main.cpp / .h
- `cs_main` → the global recursive mutex protecting chainstate and block index access

### src/kernel/digibytekernel.cpp
- Kernel library entry point for standalone chainstate validation (without full node)

### src/kernel/disconnected_transactions.h
- `DisconnectedBlockTransactions` (class) → pool of transactions from disconnected blocks during reorg, resubmitted to mempool after reorg completes

### src/kernel/mempool_entry.h
- `CTxMemPoolEntry` (class) → a transaction in the mempool with metadata: fee, size, height, time, ancestor/descendant counts and fees
  - `GetTx()` → returns the transaction reference
  - `GetFee()` → returns the transaction fee
  - `GetTxSize()` → returns virtual transaction size
  - `GetModifiedFee()` → returns fee with priority adjustments
  - `GetTime()` → returns when the transaction entered the mempool

### src/kernel/mempool_limits.h
- `MemPoolLimits` (struct) → ancestor/descendant count and size limits for mempool packages

### src/kernel/mempool_options.h
- `MemPoolOptions` (struct) → mempool configuration: max size, expiry time, min relay fee, limits

### src/kernel/mempool_persist.cpp / .h
- `DumpMempool()` → saves mempool contents to `mempool.dat` for persistence across restarts
- `LoadMempool()` → loads mempool from `mempool.dat` on startup

### src/kernel/mempool_removal_reason.cpp / .h
- `MemPoolRemovalReason` enum → why a tx was removed: EXPIRY, SIZELIMIT, REORG, BLOCK, CONFLICT, REPLACED
- `RemovalReasonToString()` → converts removal reason to display string

### src/kernel/messagestartchars.h
- `MessageStartChars` (array) → 4-byte magic bytes identifying DigiByte network messages (differs per network)

### src/kernel/notifications_interface.h
- `kernel::Notifications` (class) → abstract interface for kernel notifications (header tip, block tip, progress, warning, fatal error)

### src/kernel/validation_cache_sizes.h
- `ValidationCacheSizes` (struct) → sizes for script and signature verification caches

---

## Source Files — src/logging/

### src/logging/timer.h
- `BCLog::Timer` (class) → RAII timer that logs elapsed time with a message on destruction (for profiling code sections)

---

## Source Files — src/node/

### src/node/abort.cpp / .h
- `AbortNode()` → triggers node abort with error message, sets exit status, optionally initiates shutdown

### src/node/blockmanager_args.cpp / .h
- `ApplyArgsManOptions()` → reads block storage config from ArgsManager into BlockManagerOpts

### src/node/blockstorage.cpp / .h
- `BlockTreeDB` (class extends CDBWrapper) → LevelDB database for block index (maps block hash → disk position + metadata)
  - `ReadBlockFileInfo()` / `WriteBlockFileInfo()` → per-file metadata
  - `WriteBatchSync()` → atomic batch write with sync
  - `LoadBlockIndexGuts()` → reads entire block index from LevelDB into memory on startup
- `BlockManager` (class) → manages block and undo file storage on disk
  - `LoadBlockIndex()` → loads full block index from database
  - `ReadBlockFromDisk()` / `ReadRawBlockFromDisk()` → reads a block from blk*.dat files
  - `ReadBlockUndo()` → reads block undo data from rev*.dat files
  - `SaveBlockToDisk()` → writes a new block to disk, allocating space as needed
  - `PruneOneBlockFile()` → deletes a block file during pruning
  - `FindBlockPos()` → finds or allocates space in block files for a new block
  - `GetBlockFileInfo()` → returns metadata for a specific block file
  - `LookupBlockIndex()` → finds a block index entry by hash
  - `AddToBlockIndex()` → creates new block index entry
- `ImportBlocks()` → imports blocks from external files during `-loadblock`

### src/node/caches.cpp / .h
- `CalculateCacheSizes()` → distributes available cache memory between UTXO DB, UTXO set, and block index

### src/node/chainstate.cpp / .h
- `LoadChainstate()` → loads or creates chainstate databases, initializes UTXO set
- `VerifyLoadedChainstate()` → verifies blockchain database integrity on startup
- `ChainstateLoadOptions` (struct) → options for chainstate loading (reindex, prune, assume-valid, etc.)

### src/node/chainstatemanager_args.cpp / .h
- `ApplyArgsManOptions()` → reads chainstate config from ArgsManager into ChainstateManagerOpts

### src/node/coin.cpp / .h
- `FindCoins()` → looks up coins from both UTXO set and mempool (for RPC)

### src/node/coinstats.cpp / .h
- `GetUTXOStats()` → computes UTXO set statistics (total supply, UTXO count, hash) with interrupt support

### src/node/coins_view_args.cpp / .h
- `ApplyArgsManOptions()` → reads UTXO cache config from ArgsManager

### src/node/connection_types.cpp / .h
- `ConnectionType` enum → peer connection types: INBOUND, OUTBOUND_FULL_RELAY, MANUAL, FEELER, BLOCK_RELAY, ADDR_FETCH

### src/node/context.cpp / .h
- `NodeContext` (struct) → aggregate of all node subsystem pointers: chainman, mempool, connman, banman, peerman, scheduler, wallet interfaces, indexes
  - Central dependency injection container for the node

### src/node/database_args.cpp / .h
- `ApplyArgsManOptions()` → reads database config options from ArgsManager

### src/node/eviction.cpp / .h
- `ProtectEvictionCandidatesByRatio()` → implements peer eviction protection logic: protects peers by network diversity, ping latency, transaction/block relay contribution, and connection age

### src/node/interfaces.cpp
- `NodeImpl` (class implements interfaces::Node) → connects the abstract Node interface to the real node subsystems
- `ChainImpl` (class implements interfaces::Chain) → connects the abstract Chain interface to chainstate
- `MakeNode()` / `MakeChain()` → factory functions

### src/node/interface_ui.cpp / .h
- `CClientUIInterface` (class) → signal-based callback system for displaying messages to the user (GUI or console)
  - `ThreadSafeMessageBox()` → shows a message box (or logs in daemon mode)
  - `InitMessage()` → shows initialization progress messages
- `InitWarning()` / `InitError()` → global functions for startup warnings/errors

### src/node/kernel_notifications.cpp / .h
- `KernelNotifications` (class implements kernel::Notifications) → bridges kernel notifications to node UI
  - `headerTip()` → shows header sync progress
  - `progress()` → shows verification/IBD progress
  - `warning()` → displays warnings
  - `fatalError()` → handles fatal errors with shutdown

### src/node/mempool_args.cpp / .h
- `ApplyArgsManOptions()` → reads mempool config from ArgsManager into MemPoolOptions

### src/node/mempool_persist_args.cpp / .h
- `ShouldPersistMempool()` → checks if mempool persistence is enabled
- `MempoolPath()` → returns the path to mempool.dat

### src/node/miner.cpp / .h
- `BlockAssembler` (class) → constructs block templates for mining by selecting transactions from the mempool
  - `CreateNewBlock()` → builds a complete block template with coinbase, selected transactions, and algo-specific header fields
  - `addPackageTxs()` → greedily selects highest-feerate transaction packages from the mempool
  - `TestPackage()` → checks if adding a package would exceed block size/sigop limits
  - `AddToBlock()` → adds a transaction to the block template
- `UpdateTime()` → updates block header timestamp, recalculates difficulty for the target mining algorithm
- `IncrementExtraNonce()` → updates coinbase extra nonce and regenerates Merkle root for mining iterations
- `RegenerateCommitments()` → regenerates SegWit witness commitment in coinbase
- `ApplyArgsManOptions()` → reads miner config (block max weight, priority) from args

### src/node/mini_miner.cpp / .h
- `MiniMiner` (class) → lightweight mempool fee-rate calculator for coin selection (simulates block assembly without full block template)
  - `CalculateBumpFees()` → calculates the fee bump needed for each UTXO to make its ancestor package attractive to miners
- `MiniMinerMempoolEntry` (class) → simplified mempool entry for MiniMiner calculations

### src/node/minisketchwrapper.cpp / .h
- Wrapper around the minisketch library for Erlay transaction reconciliation (BIP 330)

### src/node/peerman_args.cpp / .h
- `ApplyArgsManOptions()` → reads peer manager config from ArgsManager

### src/node/psbt.cpp / .h
- `AnalyzePSBT()` → analyzes a PSBT and returns per-input signing status, estimated fees, and next required action

### src/node/transaction.cpp / .h
- `BroadcastTransaction()` → validates and broadcasts a transaction to the P2P network
- `GetTransaction()` → retrieves a transaction from mempool or on-disk block data

### src/node/txreconciliation.cpp / .h
- `TxReconciliationTracker` (class) → manages Erlay-style transaction reconciliation state with peers (BIP 330)
  - `RegisterPeer()` → initializes reconciliation state for a peer
  - `ForgetPeer()` → cleans up reconciliation state for disconnected peer

### src/node/ui_interface.cpp / .h
- Legacy UI interface forwarding (signals for block notifications, progress, etc.)

### src/node/utxo_snapshot.cpp / .h
- `SnapshotMetadata` (class) → metadata for assumeUTXO snapshots (block hash, coin count)
- `WriteSnapshotBaseBlockhash()` / `ReadSnapshotBaseBlockhash()` → persists the snapshot base block
- `FindSnapshotChainstateDir()` → locates snapshot chainstate directory

### src/node/validation_cache_args.cpp / .h
- `ApplyArgsManOptions()` → reads signature/script cache size config from ArgsManager

---

## Source Files — src/policy/

### src/policy/feerate.cpp / .h
- `CFeeRate` (class) → represents a fee rate in satoshis per kilobyte (or per kvB)
  - `GetFee()` → calculates fee for a given transaction size
  - `ToString()` → human-readable fee rate string
- `FeeEstimateMode` enum → UNSET, ECONOMICAL, CONSERVATIVE

### src/policy/fees.cpp / .h
- `CBlockPolicyEstimator` (class) → estimates optimal fee rates based on historical confirmation times
  - `estimateSmartFee()` → returns fee estimate for target confirmation blocks with confidence level
  - `estimateRawFee()` → returns raw fee estimate for a specific time horizon
  - `processBlock()` → updates estimates with newly confirmed transactions
  - `processTransaction()` → records a new unconfirmed transaction for tracking
  - `FlushUnconfirmed()` → clears expired unconfirmed transaction tracking data
- `FeeFilterRounder` (class) → rounds fee rates to reduce fingerprinting via feefilter messages
- `TxConfirmStats` (class) → statistical buckets tracking confirmation times by fee rate

### src/policy/fees_args.cpp / .h
- `ApplyArgsManOptions()` → reads fee estimation config from ArgsManager

### src/policy/packages.cpp / .h
- `CheckPackage()` → validates a transaction package: no duplicates, reasonable count/size, valid topology
- `IsChildWithParents()` → checks if package is a single child with all its direct parents
- `IsChildWithParentsTree()` → validates child-with-parents tree structure for package relay
- `PackageValidationState` (class) → carries package-level validation results

### src/policy/policy.cpp / .h
- `IsStandardTx()` → checks if a transaction meets relay/mining standardness rules (version, size, script types, dust)
- `AreInputsStandard()` → validates transaction inputs use standard script forms
- `IsWitnessStandard()` → validates witness programs conform to known versions
- `GetDustThreshold()` → calculates minimum output value to avoid being considered dust
- `IsDust()` → checks if an output is below the dust threshold
- `GetVirtualTransactionSize()` → converts weight to virtual bytes (weight/4 rounded up)
- Key constants: `MAX_STANDARD_TX_WEIGHT`, `MAX_P2SH_SIGOPS`, `DEFAULT_MAX_MEMPOOL_SIZE_MB`, `DUST_RELAY_TX_FEE`

### src/policy/rbf.cpp / .h
- `IsRBFOptIn()` → checks if a transaction signals replace-by-fee (BIP 125: any input with nSequence < 0xfffffffe)
- `IsRBFOptInEmptyMempool()` → checks RBF signal without mempool context (for new transactions)
- `RBFTransactionState` enum → UNKNOWN, REPLACEABLE_BIP125, FINAL

### src/policy/settings.cpp / .h
- `fIsBareMultisigStd` → global setting for whether bare multisig is standard
- `nBytesPerSigOp` → sigop cost accounting factor
- `dustRelayFee` → fee rate used for dust threshold calculation
- `incrementalRelayFee` → minimum fee increment for mempool replacement

---

## Source Files — src/primitives/

### src/primitives/block.cpp / .h
- `CBlockHeader` (class) → block header: version, prev hash, merkle root, timestamp, nBits (difficulty), nNonce
  - `GetHash()` → double-SHA256 hash of the header (block hash)
  - `GetPoWAlgo()` → extracts mining algorithm from version field (bits 8-10 encode algo 0-4)
- `CBlock` (class extends CBlockHeader) → full block: header + vector of transactions
  - `ToString()` → human-readable block summary
- `GetAlgoName()` → maps algo number (0-7) to name string ("sha256d", "scrypt", "groestl", "skein", "qubit", "odocrypt")
- `GetAlgoByName()` → reverse mapping from name to algo number
- `GetVersionForAlgo()` → constructs version field with algo bits set
- `OdoKey()` → derives time-rotating Odocrypt key from block timestamp

### src/primitives/transaction.cpp / .h
- `COutPoint` (class) → transaction output reference: txid + output index (vout)
- `CTxIn` (class) → transaction input: outpoint + scriptSig + nSequence + witness
- `CTxOut` (class) → transaction output: amount (nValue) + scriptPubKey
- `CTransaction` (class) → immutable transaction with cached hash and witness hash
  - `GetHash()` → returns txid (hash without witness data)
  - `GetWitnessHash()` → returns wtxid (hash including witness data)
  - `IsCoinBase()` → checks if this is a coinbase transaction
  - `HasWitness()` → checks if any input has witness data
  - `GetValueOut()` → sums all output values
- `CMutableTransaction` (class) → mutable version of CTransaction for building/modifying transactions
- `GenTxid` (class) → generic transaction identifier (txid or wtxid)
- ⚠️ `DigiDollarTxType` enum → DD transaction types (NONE, MINT, TRANSFER, REDEEM)
- ⚠️ `IsDigiDollarTransaction()` → checks if transaction has DD type flags in version field
- ⚠️ `GetDigiDollarTxType()` → extracts DD transaction type from version
- ⚠️ `MakeDigiDollarVersion()` → encodes DD type and flags into transaction version
- ⚠️ `GetDigiDollarTxTypeName()` → human-readable DD transaction type name

### src/primitives/oracle.cpp / .h
- ⚠️ `COraclePriceMessage` (class) → signed oracle price report: DGB/USD price, timestamp, oracle ID, Schnorr signature
  - `GetHash()` → message hash for signature verification
  - `VerifySignature()` → verifies Schnorr signature against oracle's public key
  - `IsExpired()` → checks if price message has expired based on block height
- ⚠️ `COracleBundle` (class) → collection of oracle price messages forming a consensus price
  - `GetMedianPrice()` → computes median price from all valid messages in bundle
  - `Validate()` → validates bundle completeness, signatures, and consistency
  - `GetEpoch()` → returns the oracle epoch this bundle belongs to
- ⚠️ `OracleNodeInfo` (struct) → oracle identity: ID, public key, and status
- ⚠️ `SelectOraclesForEpoch()` → deterministically selects which oracles are active in a given epoch
- ⚠️ `GetCurrentEpoch()` → calculates current oracle epoch from block height

---

## Source Files — src/qt/ (Lighter Coverage)

The Qt GUI provides graphical interface for DigiByte Core. Key components:

- `digibyte.cpp` → GUI application entry point, initializes Qt and node
- `digibyteamountfield.cpp` → input widget for DGB amounts with unit switching
- `digibyteunits.cpp` → DGB unit conversion (DGB, mDGB, µDGB, sat)
- `digibyteaddressvalidator.cpp` → validates DigiByte addresses in input fields
- `walletmodel.cpp` → bridges CWallet to Qt model for display/interaction
- `clientmodel.cpp` → bridges node state (peers, blocks, sync progress) to Qt model
- `sendcoinsdialog.cpp` → send coins dialog with address, amount, fee controls
- `receivecoinsdialog.cpp` → generate receive addresses with QR codes
- `transactiontablemodel.cpp` → displays transaction history in table view
- `overviewpage.cpp` → main wallet overview (balances, recent transactions)
- `optionsdialog.cpp` → node settings dialog (network, wallet, display)
- `rpcconsole.cpp` → built-in RPC console and peer info panel
- `paymentserver.cpp` → BIP 21 URI and payment protocol handler
- `notificator.cpp` → OS-native desktop notifications
- `splashscreen.cpp` → startup splash with initialization progress
- `guiutil.cpp` → shared GUI utility functions (clipboard, file dialogs, formatting)

---

## Source Files — src/rpc/

### src/rpc/blockchain.cpp / .h
- RPC commands: `getblockcount`, `getbestblockhash`, `getblockhash`, `getblockheader`, `getblock`, `getblockchaininfo`, `getchaintips`, `getdifficulty`, `getblockstats`, `gettxoutsetinfo`, `gettxout`, `verifychain`, `preciousblock`, `invalidateblock`, `reconsiderblock`, `waitfornewblock`, `waitforblock`, `waitforblockheight`, `syncwithvalidationinterfacequeue`, `getblockfrompeer`, `dumptxoutset`, `scanblocks`
- `GetDifficulty()` → calculates human-readable difficulty value, supports per-algo difficulty queries
- `blockToJSON()` → converts CBlock to detailed JSON representation
- `blockheaderToJSON()` → converts block header to JSON
- `MempoolInfoToJSON()` / `MempoolToJSON()` → mempool state as JSON
- `EnsureChainman()` / `EnsureMemPool()` / `EnsureFeeEstimator()` → extract subsystem pointers from RPC context

### src/rpc/client.cpp / .h
- `CRPCConvertTable` → maps RPC method parameters to expected types (string→int/bool/array/object)
- `ParseNonRFCJSONValue()` → parses JSON values that aren't strictly RFC-compliant

### src/rpc/external_signer.cpp
- RPC command: `enumeratesigners` → lists connected hardware wallets

### src/rpc/fees.cpp
- RPC commands: `estimatesmartfee`, `estimaterawfee` → fee estimation RPCs

### src/rpc/mempool.cpp / .h
- RPC commands: `sendrawtransaction`, `testmempoolaccept`, `getmempoolinfo`, `getrawmempool`, `getmempoolentry`, `getmempoolancestors`, `getmempooldescendants`, `submitpackage`, `savemempool`
- `MempoolEntryDescription()` → generates JSON description of a mempool entry

### src/rpc/mining.cpp / .h
- RPC commands: `getmininginfo`, `getnetworkhashps`, `generatetoaddress`, `generatetodescriptor`, `generateblock`, `getblocktemplate`, `submitblock`, `submitheader`, `prioritisetransaction`, `getprioritisedtransactions`
- `getblocktemplate` → returns block template for external miners with algo selection support
- `getmininginfo` → returns current mining state including active algorithm info

### src/rpc/misc.cpp
- RPC commands: `validateaddress`, `createmultisig`, `getdescriptorinfo`, `deriveaddresses`, `verifymessage`, `signmessagewithprivkey`, `setmocktime`, `mockscheduler`, `getmemoryinfo`, `logging`, `getindexinfo`, `echo`

### src/rpc/net.cpp / .h
- RPC commands: `getconnectioncount`, `ping`, `getpeerinfo`, `addnode`, `disconnectnode`, `getaddednodeinfo`, `getnettotals`, `getnetworkinfo`, `setban`, `listbanned`, `clearbanned`, `setnetworkactive`, `addconnection`, `getnodeaddresses`, `getaddrmaninfo`

### src/rpc/node.cpp
- RPC commands: `stop`, `uptime`, `getmemoryinfo`

### src/rpc/output_script.cpp
- RPC commands: `getbestblockhash`, `decodescript` (output script analysis)

### src/rpc/protocol.h
- `RPCErrorCode` enum → all JSON-RPC error codes (INVALID_REQUEST, METHOD_NOT_FOUND, PARSE_ERROR, etc.)
- JSON-RPC protocol constants and request/response structures

### src/rpc/rawtransaction.cpp / .h
- RPC commands: `getrawtransaction`, `createrawtransaction`, `decoderawtransaction`, `decodescript`, `combinerawtransaction`, `signrawtransactionwithkey`, `sendrawtransaction`, `testmempoolaccept`
- PSBT RPCs: `decodepsbt`, `combinepsbt`, `finalizepsbt`, `createpsbt`, `converttopsbt`, `utxoupdatepsbt`, `joinpsbts`, `analyzepsbt`

### src/rpc/rawtransaction_util.cpp / .h
- `ConstructTransaction()` → builds CMutableTransaction from JSON inputs/outputs specification
- `AddInputs()` / `AddOutputs()` → adds inputs/outputs from JSON to a mutable transaction
- `ParsePrevouts()` → parses previous output info for offline transaction signing
- `SignTransaction()` → signs a transaction using provided keys

### src/rpc/register.h
- `RegisterAllCoreRPCCommands()` → registers all core RPC command groups (blockchain, mining, net, mempool, misc, rawtransaction, signer)
- Individual registration functions for each RPC module

### src/rpc/request.cpp / .h
- `JSONRPCRequest` (class) → parsed JSON-RPC request with method, params, auth context
- `JSONRPCReply()` → constructs a JSON-RPC response object
- `JSONRPCError()` → constructs a JSON-RPC error response

### src/rpc/server.cpp / .h
- `CRPCTable` (class) → maps RPC method names to handler functions
  - `execute()` → dispatches an RPC request to the appropriate handler
  - `appendCommand()` → registers a new RPC command
  - `listCommands()` → returns all registered command names
- `StartRPC()` / `InterruptRPC()` / `StopRPC()` → RPC lifecycle management
- `IsRPCRunning()` → checks if RPC server is active
- `SetRPCWarmupStatus()` / `SetRPCWarmupFinished()` → manages warmup state during startup
- `RPCRunLater()` → schedules a one-shot RPC callback for later execution

### src/rpc/server_util.cpp / .h
- Helper functions for extracting node subsystem references from RPC context

### src/rpc/signmessage.cpp
- RPC command: `signmessagewithprivkey` → signs a message with a provided private key

### src/rpc/txoutproof.cpp
- RPC commands: `gettxoutproof` (creates Merkle proof for tx inclusion), `verifytxoutproof` (verifies Merkle proof)

### src/rpc/util.cpp / .h
- `RPCHelpMan` (class) → self-documenting RPC command with parameter validation, help text generation, and type checking
- `AmountFromValue()` → converts JSON value to CAmount with validation
- `ParseHashV()` / `ParseHashO()` → parses hex hash from JSON
- `HexToPubKey()` / `AddrToPubKey()` → converts hex/address to CPubKey
- `DescribeAddress()` → generates JSON description of an address
- `RPCErrorFromTransactionError()` → maps transaction errors to RPC error codes

---

## Source Files — src/script/

### src/script/descriptor.cpp / .h
- `Descriptor` (abstract class) → output descriptor: human-readable script template (BIP 380-386)
  - `Expand()` → generates scriptPubKeys and signing info for given key range
  - `ExpandFromCache()` → expands using cached derived keys (no private key access needed)
  - `IsSolvable()` → checks if descriptor can produce signed transactions
  - `IsRange()` → checks if descriptor uses wildcards (e.g., `pkh(xpub.../*)`)
  - `ToString()` / `ToPrivateString()` → serializes descriptor with optional private key export
- `DescriptorCache` (class) → caches expanded keys to avoid repeated derivation
- `Parse()` → parses a descriptor string into a Descriptor object
- `InferDescriptor()` → infers a descriptor from a script and signing provider
- `GetDescriptorChecksum()` → computes descriptor checksum (8-character suffix)
- `DescriptorID()` → computes a unique ID for a descriptor

### src/script/digibyteconsensus.cpp / .h
- `digibyteconsensus_verify_script()` → C API for script verification (shared library export)
- `digibyteconsensus_version()` → returns consensus library version

### src/script/interpreter.cpp / .h
- `EvalScript()` → executes a Bitcoin script on the stack machine, handling all opcodes including SegWit v0 and Tapscript
- `VerifyScript()` → full script verification: evaluates scriptSig, scriptPubKey, and witness programs
- `BaseSignatureChecker` (abstract class) → interface for signature verification
- `GenericTransactionSignatureChecker<T>` (class) → verifies ECDSA and Schnorr signatures against transaction data
  - `CheckSig()` → verifies ECDSA signature for legacy/SegWit v0 scripts
  - `CheckSchnorrSignature()` → verifies BIP340 Schnorr signature for Taproot
  - `CheckLockTime()` / `CheckSequence()` → validates OP_CHECKLOCKTIMEVERIFY and OP_CHECKSEQUENCEVERIFY
- `CachingTransactionSignatureChecker` → see `src/script/sigcache.h`
- `SignatureHash()` → computes the sighash for ECDSA signing (BIP 143 for SegWit)
- `SignatureHashSchnorr()` → computes the sighash for Schnorr signing (BIP 341/342)
- `CheckSignatureEncoding()` → validates DER signature encoding (BIP 66)
- `ComputeTapleafHash()` / `ComputeTapbranchHash()` → Taproot tree hash computations
- `ComputeTaprootMerkleRoot()` → verifies Taproot control block against expected Merkle root
- `CountWitnessSigOps()` → counts signature operations in witness programs
- Script flags: `SCRIPT_VERIFY_P2SH`, `SCRIPT_VERIFY_WITNESS`, `SCRIPT_VERIFY_TAPROOT`, etc.

### src/script/ismine.cpp / .h
- `IsMine()` → determines if a script/destination belongs to a keystore (ISMINE_SPENDABLE, ISMINE_WATCH_ONLY, ISMINE_NO)
- `isminetype` enum → NO, WATCH_ONLY, SPENDABLE, ALL

### src/script/miniscript.cpp / .h
- `miniscript::Node<Key>` (class template) → Miniscript abstract syntax tree node for policy compilation
- `miniscript::Type` (class) → Miniscript type system for correctness/malleability analysis
- `Fragment` enum → all Miniscript fragments (pk, pkh, older, after, sha256, thresh, and_v, or_b, etc.)
- Miniscript contexts: P2WSH and P2TR Tapscript

### src/script/script.cpp / .h
- `CScript` (class extends vector<uint8_t>) → serialized Bitcoin script (sequence of opcodes and data pushes)
  - `IsPayToScriptHash()` → checks if script is P2SH pattern
  - `IsPayToWitnessScriptHash()` → checks if script is P2WSH pattern
  - `IsWitnessProgram()` → checks if script is any witness program (SegWit)
  - `IsPushOnly()` → validates script contains only data push operations
  - `GetSigOpCount()` → counts signature operations in the script
  - `HasValidOps()` → checks all opcodes are defined
  - `FindAndDelete()` → removes a specific byte pattern from script (for CODESEPARATOR)
- `CScriptNum` (class) → Bitcoin script number: variable-length signed integer with overflow detection
- `GetOpName()` → returns human-readable opcode name (e.g., "OP_DUP", "OP_CHECKSIG")
- `CScriptID` (class) → Hash160 of a script, used for P2SH addresses

### src/script/sigcache.cpp / .h
- `CachingTransactionSignatureChecker` (class) → signature checker with cuckoo-cache for verified signatures
  - Avoids re-verifying signatures already seen (significant speedup during block validation)

### src/script/sign.cpp / .h
- `ProduceSignature()` → creates a complete signature for a script using the given signing provider
- `SignTransaction()` → signs all inputs of a mutable transaction
- `MutableTransactionSignatureCreator` (class) → creates signatures for transaction inputs with sighash computation
- `DataFromTransaction()` → extracts existing signature data from a transaction input
- `UpdateInput()` → applies signature data to a transaction input
- `IsSegWitOutput()` → checks if an output requires SegWit spending

### src/script/signingprovider.cpp / .h
- `SigningProvider` (abstract class) → interface for accessing keys, scripts, and key origin info needed for signing
  - `GetCScript()` / `GetPubKey()` / `GetKey()` / `GetKeyOrigin()` / `GetTaprootSpendData()`
- `FillableSigningProvider` (class) → in-memory signing provider that can add keys and scripts
  - `AddKey()` → stores a private key
  - `AddCScript()` → stores a redeemScript
  - `HaveKey()` / `HaveCScript()` → checks for key/script availability
- `HidingSigningProvider` (class) → wraps another provider, hiding private keys or scripts
- `MultiSigningProvider` (class) → chains multiple providers, trying each in order
- `GetKeyForDestination()` → resolves destination to the signing key ID

### src/script/solver.cpp / .h
- `Solver()` → classifies a scriptPubKey into its type and extracts embedded data (pubkeys, hashes, witness programs)
- `TxoutType` enum → NONSTANDARD, PUBKEY, PUBKEYHASH, SCRIPTHASH, MULTISIG, NULL_DATA, WITNESS_V0_KEYHASH, WITNESS_V0_SCRIPTHASH, WITNESS_V1_TAPROOT, WITNESS_UNKNOWN
- `GetTxnOutputType()` → converts TxoutType enum to human-readable string
- `GetScriptForRawPubKey()` → creates P2PK script from a public key
- `GetScriptForMultisig()` → creates multisig script from threshold + pubkeys
- `MatchMultiA()` → detects Tapscript multi_a() pattern

### src/script/standard.cpp / .h
- `ExtractDestination()` → extracts a single CTxDestination from a scriptPubKey
- `ExtractDestinations()` → extracts all destinations from multisig or complex scripts
- `GetScriptForDestination()` → converts CTxDestination to corresponding scriptPubKey
- `TaprootBuilder` (class) → constructs Taproot output keys from internal key + script tree
  - `Add()` → adds a script leaf at a given depth
  - `Finalize()` → computes the output key and spend data
  - `IsComplete()` → checks if the tree is fully specified
  - `GetOutput()` → returns the final Taproot output key
  - `GetSpendData()` → returns all spend paths (key path + script paths with control blocks)
- `InferTaprootTree()` → reconstructs a Taproot tree structure from spend data

---

## Source Files — src/support/

### src/support/cleanse.cpp / .h
- `memory_cleanse()` → securely zeroes memory (resistant to compiler optimization, for key material)

### src/support/events.h
- RAII wrappers for libevent objects (`evhttp`, `evhttp_request`, `event_base`)

### src/support/lockedpool.cpp / .h
- `LockedPoolManager` (class) → singleton managing secure memory allocation (mlock'd pages that can't be swapped to disk)
- `LockedPool` (class) → allocator that locks memory pages to prevent sensitive data (keys) from being written to swap
  - `alloc()` / `free()` → allocate/free locked memory
- `Arena` (class) → memory arena with chunk management for the locked pool

---

## Source Files — src/util/

### src/util/asmap.cpp / .h
- `DecodeAsmap()` → decodes compressed ASN (Autonomous System Number) map for peer bucketing by AS instead of /16

### src/util/batchpriority.cpp / .h
- `ScheduleBatchPriority()` → sets current thread to low scheduling priority (for background index sync)

### src/util/bip32.cpp / .h
- `FormatHDKeypath()` → formats BIP32 derivation path as string (e.g., "m/84'/20'/0'/0/0")
- `ParseHDKeypath()` → parses derivation path string into vector of child indices
- `WriteHDKeypath()` → writes HD keypath to a stream

### src/util/bytevectorhash.cpp / .h
- `ByteVectorHash` (class) → SipHash-based hasher for byte vectors in hash maps

### src/util/chaintype.cpp / .h
- `ChainType` enum → MAIN, TESTNET, SIGNET, REGTEST
- `ChainTypeFromString()` → parses chain type from string
- `ChainTypeToString()` → converts chain type to string

### src/util/check.cpp / .h
- `Assert()` → assertion that aborts with backtrace in debug builds
- `Assume()` → soft assertion that logs but doesn't abort in release builds

### src/util/epochguard.h
- `Epoch` (class) → epoch-based RAII guard for efficient "mark and sweep" operations on data structures

### src/util/error.cpp / .h
- `TransactionError` enum → ALREADY_IN_CHAIN, MEMPOOL_REJECTED, MEMPOOL_ERROR, MAX_FEE_EXCEEDED, etc.
- `TransactionErrorString()` → human-readable error messages for transaction submission failures
- `ResolveErrMsg()` → generates error messages for name/address resolution failures

### src/util/exception.cpp / .h
- `PrintExceptionContinue()` → logs exception details and optionally continues execution

### src/util/fees.cpp / .h
- `StringForFeeReason()` → converts fee reason enum to display string
- `FeeModeFromString()` → parses fee estimate mode from string ("economical", "conservative")

### src/util/fs.cpp / .h
- Filesystem utilities wrapping `std::filesystem` with DigiByte-specific path handling
- `fs::path` → filesystem path type used throughout the codebase

### src/util/fs_helpers.cpp / .h
- `RenameOver()` → atomic file rename (cross-platform)
- `LockDirectory()` / `UnlockDirectory()` → directory locking via .lock files
- `DirIsWritable()` → checks directory write permissions
- `AllocateFileRange()` → pre-allocates file space on disk (platform-specific)
- `ReleaseDirectoryLocks()` → releases all directory locks on shutdown

### src/util/getuniquepath.cpp / .h
- `GetUniquePath()` → generates unique temporary file path

### src/util/golombrice.h
- `GolombRiceDecode()` / `GolombRiceEncode()` → Golomb-Rice coding for BIP 158 compact block filters

### src/util/hasher.cpp / .h
- `SaltedTxidHasher` / `SaltedOutpointHasher` → randomized hashers for hash tables (DoS-resistant)
- `FilterHeaderHasher` / `SignatureCacheHasher` → specialized hashers for specific caches

### src/util/message.cpp / .h
- `MessageSign()` → signs a message with a private key (Bitcoin signed message format)
- `MessageVerify()` → verifies a signed message against an address
- `MessageHash()` → computes the hash of a message with the "DigiByte Signed Message" prefix

### src/util/moneystr.cpp / .h
- `FormatMoney()` → formats CAmount as human-readable string with 8 decimal places
- `ParseMoney()` → parses decimal string to CAmount

### src/util/rbf.cpp / .h
- `SignalsOptInRBF()` → checks if a transaction signals opt-in RBF

### src/util/readwritefile.cpp / .h
- `ReadBinaryFile()` / `WriteBinaryFile()` → simple binary file I/O

### src/util/result.h
- `util::Result<T>` → result type carrying either a success value or a bilingual error message

### src/util/serfloat.cpp / .h
- `EncodeDouble()` / `DecodeDouble()` → platform-independent IEEE 754 double serialization

### src/util/settings.cpp / .h
- `ReadSettings()` / `WriteSettings()` → persistent settings file I/O
- `GetSetting()` → retrieves a setting with priority resolution

### src/util/signalinterrupt.cpp / .h
- `SignalInterrupt` (class) → thread-safe interrupt flag using eventfd (Linux) or pipe for clean shutdown signaling

### src/util/sock.cpp / .h
- `Sock` (class) → RAII wrapper around OS socket descriptor with send/recv/wait operations
  - `Send()` / `Recv()` → socket I/O with error handling
  - `Wait()` → polls socket for readability/writability with timeout
  - `WaitMany()` → polls multiple sockets simultaneously

### src/util/spanparsing.cpp / .h
- `Const()` / `Func()` / `Expr()` → lightweight parser combinators for descriptor string parsing

### src/util/strencodings.cpp / .h
- `HexStr()` → converts bytes to hex string
- `ParseHex()` / `TryParseHex()` → converts hex string to bytes
- `EncodeBase32()` / `DecodeBase32()` → base32 encoding/decoding (for Tor addresses)
- `EncodeBase64()` / `DecodeBase64()` → base64 encoding/decoding
- `SanitizeString()` → removes non-printable characters from strings
- `IsHex()` / `IsHexNumber()` → validates hex strings
- `atoi64()` / `LocaleIndependentAtoi()` → safe string-to-integer conversion

### src/util/string.cpp / .h
- `TrimString()` → trims whitespace/specified characters from string
- `FormatParagraph()` → word-wraps text to specified width
- `Join()` → joins container elements with separator
- `ContainsNoNUL()` → validates string has no embedded null bytes
- `RemovePrefix()` / `RemovePrefixView()` → removes a prefix from a string

### src/util/syserror.cpp / .h
- `SysErrorString()` → converts system errno to human-readable string

### src/util/system.cpp / .h
- Legacy system utilities (most moved to common/system.h)

### src/util/thread.cpp / .h
- `TraceThread()` → wrapper that runs a function in a named thread with exception logging

### src/util/threadinterrupt.cpp / .h
- `CThreadInterrupt` (class) → interruptible sleep mechanism for background threads
  - `sleep_for()` → sleeps for a duration, returning early if interrupted
  - `interrupt()` → wakes all sleeping threads

### src/util/threadnames.cpp / .h
- `SetSelfThreadName()` → sets the OS-level name for the current thread (for debugging)
- `GetThreadName()` → retrieves the current thread's name

### src/util/time.cpp / .h
- `GetTime()` → returns current Unix timestamp (mockable for testing)
- `GetTimeMillis()` / `GetTimeMicros()` → high-resolution timestamps
- `SetMockTime()` → overrides system time for testing
- `MillisToString()` → formats milliseconds as human-readable duration
- `FormatISO8601DateTime()` / `FormatISO8601Date()` → ISO 8601 date formatting
- `ParseISO8601DateTime()` → parses ISO 8601 date string to timestamp

### src/util/tokenpipe.cpp / .h
- `TokenPipe` (class) → one-way byte pipe for inter-thread token passing (used for process synchronization)

### src/util/translation.h
- `bilingual_str` (struct) → holds both original English and translated error/warning messages
- `_()` → marks a string for translation (gettext-compatible)
- `Untranslated()` → wraps an English-only string

### src/util/url.cpp / .h
- `urlDecode()` → URL percent-decoding

### src/util/vector.h
- `Cat()` → concatenates vectors
- `Vector()` → constructs vector from arguments

---

## Source Files — src/wallet/

### src/wallet/bdb.cpp / .h
- `BerkeleyEnvironment` (class) → manages Berkeley DB environment (shared across wallets in same directory)
- `BerkeleyDatabase` (class extends WalletDatabase) → BDB-backed wallet database (legacy format)
- `BerkeleyBatch` (class extends DatabaseBatch) → RAII BDB transaction batch
- `BerkeleyCursor` (class extends DatabaseCursor) → BDB database cursor
- `BerkeleyDatabaseVersion()` → returns BDB library version string
- `BerkeleyDatabaseSanityCheck()` → validates BDB library compatibility

### src/wallet/coincontrol.cpp / .h
- `CCoinControl` (class) → user preferences for coin selection: manually selected inputs,