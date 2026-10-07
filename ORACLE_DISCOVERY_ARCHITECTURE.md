# Oracle Discovery Architecture

> **Status:** Design proposal. The features described in Layers 1-3 (on-chain registry, DNS seeds, endpoint gossip) are NOT implemented. Today's discovery is the pre-Layer-1 baseline described under "Current Implementation" below: static chainparams metadata plus ordinary oracle P2P relay. Pull-on-demand of missing oracle price telemetry from peers via `getoracles` exists, but there is no decentralized endpoint discovery message.

## Current Implementation (V1, code as shipped)

`src/kernel/chainparams.cpp` populates `vOracleNodes` for each network. Each
entry contains a compressed pubkey, an `endpoint` string, and an `is_active`
flag. Mainnet and testnet26 declare 35 active metadata slots, with slots 0-34
in `consensus.vOraclePublicKeys` for the 7-of-35 V1 quorum. Slot 28 uses the
DigiHash Mining Pool key, slot 31 uses the Peer2Peer / DigiRoos key, and all
35 configured slots contain valid compressed secp256k1 oracle keys. Regtest
declares 7.

The `endpoint` field is informational metadata for operator coordination (it shows up in `getoracles` and `listoracle` RPC output). DigiByte Core does **not** make outbound connections to those endpoints — oracle data flows over the standard P2P graph. Wallet/light nodes therefore do not need to discover oracle endpoints to use DigiDollar; they only need a working P2P link to any peer that has the latest MuSig2 bundle.

The `getoracles` P2P message (`src/protocol.cpp:56`, handler in `src/net_processing.cpp:6343`) lets a node pull missing oracle price telemetry from a peer rather than waiting for it to be re-gossiped. It carries a request descriptor; the peer responds by re-pushing matching fresh `oracleprice` messages and recent signed `oraclehb` version heartbeats. It does not return on-chain bundles, MuSig2 nonces, context proposals, partial signatures, or endpoint records. There is no Layer-3 `oracleaddr` style endpoint announcement on the wire today.

Everything below this section is design intent for adding decentralized endpoint discovery on top of that baseline.

## The Problem (design motivation)

Today the `endpoint` strings in `vOracleNodes` are static metadata in chainparams. That has the same drawbacks any hardcoded endpoint list would:

1. **New oracles can't be added** without a software update
2. **Oracle operators need domain names** — barrier to entry
3. **Single points of failure** — if a domain goes down, the metadata is wrong
4. **Centralization risk** — whoever controls the DNS controls how operators are discovered out-of-band

**Core question:** How could wallet/operator tooling discover oracle endpoints in the future, without hardcoding URLs?

## Design Constraints

- Must work without central servers
- Must be resistant to Sybil attacks (can't just let anyone claim to be an oracle)
- Oracle public keys ARE in chainparams — that's the trust anchor
- Need to support adding new oracles over time
- Should work even if most of the network is behind NAT

## Proposed Solution: Multi-Layer Oracle Discovery

### Layer 1: On-Chain Oracle Registry (Primary)

**How it works:** Oracle operators register their endpoint information in a special OP_RETURN transaction, signed with their oracle key.

```
OP_RETURN OP_ORACLE_ENDPOINT <oracle_id> <endpoint_data> <schnorr_sig>
```

**Endpoint data format:**
- IPv4/IPv6 address + port (16 bytes)
- Optional: Tor .onion address (32 bytes)
- Optional: DNS hostname (variable length)
- Timestamp (prevents replay)

**Why this works:**
- Oracle keys are in chainparams — only authorized oracles can register
- Registration is permanent on-chain — survives node restarts
- No external infrastructure needed
- Nodes scan the chain for the latest endpoint registration per oracle_id
- Signature prevents spoofing

**Updating endpoints:** Oracle just sends a new registration TX. Nodes use the most recent one per oracle_id.

**Cost:** Minimal — one small TX per oracle when their endpoint changes. Could be free (coinbase) if they're also mining.

### Layer 2: DNS Seeds (Bootstrap)

**How it works:** Similar to Bitcoin's DNS seeds for node discovery, maintain DNS records that resolve to known oracle endpoints.

```
oracle-seeds.digibyte.io → A records pointing to oracle IPs
```

**DNS TXT records** can encode oracle_id → endpoint mappings:
```
TXT "oracle:0:203.0.113.10:12034"
TXT "oracle:1:198.51.100.20:12034"
```

**Why this works:**
- Fast bootstrap for new nodes
- Multiple DNS seed operators for redundancy
- Familiar pattern from Bitcoin node discovery
- Works even before the node has synced the chain

**Limitation:** Requires someone to maintain DNS records. But multiple community members can run seeds.

### Layer 3: P2P Gossip (Runtime Discovery)

**How it works:** Oracles announce themselves via a new P2P message type `oracleaddr`, similar to Bitcoin's `addr` message.

```
Message: oracleaddr
Payload:
  oracle_id (4 bytes)
  services (8 bytes) — what the oracle provides
  addr (16 bytes) — IPv6-mapped address
  port (2 bytes)
  timestamp (8 bytes)
  schnorr_sig (64 bytes) — signed by oracle key
```

**Flow:**
1. Oracle node starts → broadcasts `oracleaddr` to connected peers
2. Peers verify signature against chainparams oracle keys
3. Peers relay valid `oracleaddr` messages to their peers
4. Nodes cache discovered oracle endpoints
5. Periodic re-announcement (every 24 hours)

**Why this works:**
- No infrastructure needed
- Real-time discovery
- Signature prevents spoofing
- Same gossip protocol used for node discovery
- Oracles behind NAT can still announce (peers relay)

### Layer 4: Hardcoded Fallbacks (Last Resort)

Keep a small set of well-known oracle endpoints in chainparams as bootstrap fallbacks. These are only used if all other discovery methods fail.

```cpp
// Fallback oracle endpoints (used only when no other discovery works)
consensus.vOracleFallbackEndpoints = {
    {"oracle0.digibyte.io", 12034},
    {"oracle1.digibyte.io", 12034},
};
```

## Adding New Oracles Over Time

### Option A: Soft Fork Activation (Recommended)

New oracle public keys are added via BIP9-style activation:
1. Propose new oracle key in software update
2. Miners signal readiness
3. After threshold, new oracle key is active
4. New oracle operator registers endpoint via Layer 1 (on-chain)

**Advantage:** Proven mechanism, requires network consensus.

### Option B: On-Chain Governance Transaction

A super-majority of existing oracles sign a "new oracle proposal" transaction:
```
OP_RETURN OP_ORACLE_GOVERNANCE <action:ADD_ORACLE> <new_pubkey> <signatures_from_existing_oracles>
```

Requires 5-of-7 (or similar threshold) existing oracle signatures to add a new oracle.

**Advantage:** No software update needed. Existing oracles can vote on new members.
**Risk:** Oracles could collude to add compromised members. Needs careful threshold design.

### Option C: Hybrid Approach (Recommended for DigiDollar)

- **Phase 1 (Now):** Oracle keys and informational endpoint strings hardcoded in chainparams. Oracle data relays over normal P2P; endpoint discovery is not implemented.
- **Phase 2 (Post-launch):** Add on-chain oracle registry for endpoint discovery.
- **Phase 3 (Mature):** Add on-chain governance for oracle membership changes. Soft fork for major changes only.

## Implementation Priority

1. **P2P Gossip (Layer 3)** — Implement first. Oracles would announce via signed `oracleaddr` messages. No infrastructure needed once the new message and relay policy exist.

2. **On-Chain Registry (Layer 1)** — Implement second. Permanent, decentralized, trustless endpoint discovery.

3. **DNS Seeds (Layer 2)** — Easy to set up for bootstrap. Multiple community members can maintain them.

4. **On-Chain Governance (Option B)** — Future enhancement for adding/removing oracles without software updates.

## P2P Oracle Message Relay

**Current state (V1):** Oracle nodes started via `startoracle` fetch live exchange prices through `MultiExchangeAggregator` (`src/oracle/exchange.cpp`), sign attestations as `COraclePriceMessage`, and broadcast `oracleprice` (`NetMsgType::ORACLEPRICE`) over P2P. They also broadcast signed `oraclehb` version heartbeats for operator/protocol telemetry. Peers validate and relay them. MuSig2 coordination then exchanges `oracleconsns` proposals, `oracleattest` per-oracle attestations, `oramusnonce` round-1 nonces, `oramusigctx` session-context proposals, and `oramusigpsig` round-2 partial signatures (`src/oracle/musig2_*.{cpp,h}` and `src/oracle/signing_orchestrator.cpp`). The aggregator emits a single 64-byte BIP-340 Schnorr signature plus a participation bitmap, which the miner embeds in the coinbase as a v0x03 OP_ORACLE bundle. The legacy `sendoracleprice` RPC was removed as a fake-price-injection vulnerability; no operator-facing manual price entry RPC remains in any network (`submitoracleprice` does not exist in the source tree). The legacy `oraclebundle` gossip message is dropped on receipt — the bundle lives on-chain only (commit `bbb85cf363`).

**Still needed:** Endpoint discovery so wallet/light nodes can choose which oracle nodes to peer with, especially as new oracles are added without a software update. The rest of this document is the design proposal for that piece.

### P2P Message Flow (today)

```
Oracle Node                    Regular Node                  Miner Node
    |                              |                             |
    |-- oracleprice -------------->|                             |
    |                              |-- oracleprice ------------->|
    |-- oraclehb ----------------->|                             |
    |                              |                             |
    |-- oracleconsns ----------->  |                             |
    |-- oracleattest ----------->  |                             |
    |-- oramusnonce ------------>  |                             |
    |-- oramusigctx ------------>  |                             |
    |-- oramusigpsig ----------->  |                             |
    |                              |       [aggregator finishes MuSig2]
    |                              |       [embeds v0x03 bundle in coinbase]
    |                              |       [mines block]
```

### Proposed `oracleaddr` message (NOT IMPLEMENTED)

The encoding below is design-only. The repository does not declare an `oracleaddr` constant in `src/protocol.{cpp,h}` and `net_processing.cpp` does not handle it.

```
Payload:
  version (1 byte)
  oracle_id (4 bytes)
  services (8 bytes)
  endpoint_type (1 byte: IPv4, IPv6, Tor, DNS)
  endpoint_data (variable, bounded)
  port (2 bytes)
  timestamp (8 bytes)
  oracle_pubkey (32 bytes, x-only)
  schnorr_sig (64 bytes)
```

**Validation before relay (proposed):**
1. oracle_id is in range [0, ORACLE_TOTAL_COUNT)
2. oracle_pubkey matches chainparams for that oracle_id
3. Schnorr signature is valid
4. Timestamp is within acceptable range
5. No duplicate from same oracle_id in last epoch
6. Endpoint type and length are valid; no automatic outbound connection is made during validation

**Rate limiting (proposed):** Max 1 message per oracle per epoch. Reject duplicates. (For comparison, the *implemented* `oracleprice` rate limiter is 3,600 novel messages per peer per hour — silent-drop, no misbehavior penalty — see `src/net_processing.cpp:5604-5632`.)

## Summary

The oracle discovery problem is solvable with the same patterns Bitcoin uses for node discovery:
- **P2P gossip** for real-time endpoint announcements
- **On-chain registry** for permanent endpoint records
- **DNS seeds** for bootstrap
- **Hardcoded fallbacks** as last resort

The key insight: **oracle public keys are the trust anchor**. Any announcement signed by a valid oracle key is trustworthy. No domains or central coordination needed.

The P2P oracle message relay is equally important — oracle price messages, heartbeats, and MuSig2 coordination messages need to propagate through the network to reach miners and aggregating peers, not just stay on the oracle's local node.
