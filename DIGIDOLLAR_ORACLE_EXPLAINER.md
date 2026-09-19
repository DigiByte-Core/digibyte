# DigiDollar Oracle System - V1 Explainer

*Updated: 2026-05-30*
*Document Version: 6.3 - testnet26 and 7-of-35 quorum*

## V1 Summary

DigiDollar V1 uses a MuSig2 oracle network to bring a deterministic DGB/USD
price into block validation. Oracle operators fetch prices from the configured
exchange set, sign and relay off-chain attestations, run a MuSig2 signing
round, and produce one on-chain v0x03 oracle bundle for miners to place in the
coinbase.

The only production on-chain oracle bundle format is:

```text
OP_RETURN OP_ORACLE <version=0x03> <v03_data>
```

Raw v0x01 and v0x02 oracle payloads are legacy data. They are not accepted,
mined, relayed as fallback, or used to update the price cache in V1.

## Roster And Quorum

| Network | Oracle metadata slots | Consensus-active slots | Quorum |
|---------|-----------------------|------------------------|--------|
| Mainnet | 35 | 35, slots 0-34 | 7 signatures from active keyset |
| Testnet26 | 35 | 35, slots 0-34 | 7 signatures from active keyset |
| Regtest | 7 | 7, slots 0-6 | 4-of-7 |

Mainnet and testnet slots 0-34 are in `consensus.vOraclePublicKeys` and
`vOracleNodes`. Slot 28 uses the DigiHash Mining Pool key, slot 31 uses the
Peer2Peer / DigiRoos key, and slots 32-34 use final submitted operator keys.
Slot 35 is outside the configured roster.

Names are display metadata from each network's roster. Mainnet slot 0 is
`DigiByte.Io Oracle` and slot 11 is `Crypto Corner Shop`; their testnet names
are `Jared` and `hallvardo`. Names do not change operator IDs, keys or quorum.

## Operator Flow

1. The oracle node fetches DGB/USD from the active exchange fetchers in
   `src/oracle/exchange.cpp`.
2. `OracleNode::FetchMedianPrice()` requires at least three responsive exchange
   sources before publishing a price.
3. The node signs an `oracleprice` message and relays it over P2P. It also
   emits signed `oraclehb` version heartbeats every five minutes while enabled;
   heartbeats are operator/protocol telemetry, not price inputs.
4. Nodes exchange `oracleconsns` and `oracleattest` messages to agree on the
   price/timestamp tuple.
5. Operators run the MuSig2 nonce/context/partial-signature flow using
   `oramusnonce`, `oramusigctx`, and `oramusigpsig`. The context proposal
   freezes the participant set, nonce set, quote set, consensus price, and
   timestamp before partial signatures are relayed.
6. A completed session yields a 64-byte aggregate BIP-340 signature and a
   participation bitmap.
7. The miner embeds the v0x03 bundle in the coinbase only when the block
   contains a DigiDollar mint/redeem and a valid bundle is available. Transfer-only
   DD blocks do not require a block oracle bundle.

Off-chain messages are signing inputs only. They are not accepted as on-chain
oracle bundles. `getoracles` is a pull-on-demand recovery message: peers answer
with matching fresh `oracleprice` messages and recent signed `oraclehb`
heartbeats, not with on-chain bundles or MuSig2 round traffic.

## On-Chain v0x03 Data

`COracleBundle::SerializeV03Data()` defines the payload:

```text
bitmap_len                 1 byte
participation_bitmap       bitmap_len bytes
epoch                      4 bytes little-endian
median_price_micro_usd     8 bytes little-endian
timestamp                  8 bytes little-endian
aggregate_sig              64 bytes
```

The version byte is pushed separately as `0x03` before this payload. Regtest
uses a one-byte bitmap; mainnet/testnet use a five-byte bitmap for the
35-slot active roster.

## Miner Behavior

`OracleBundleManager::CreateOracleScript()` emits only v0x03 scripts. It
returns an empty script unless the bundle has:

- `version == 3`
- a 64-byte aggregate signature
- a non-empty participation bitmap
- a decodable participant set
- at least the network quorum of participants

`OracleBundleManager::AddOracleBundleToBlock()` first checks completed MuSig2
sessions, then the cached current-epoch bundle. If no valid v0x03 bundle is
ready:

- price-dependent DD mint/redeem transactions are skipped or removed from the
  template.
- price-independent DD transfers can remain valid without a block oracle
  bundle.
- ordinary non-DD block templates may omit the oracle bundle and remain valid.

## Block Validation

`OracleDataValidator::ValidateBlockOracleData()` runs after DigiDollar
activation on mainnet, testnet, and regtest. There is no mainnet validation
bypass.

Validation order:

1. If DigiDollar is not active for the historical block state, return true.
2. Count `OP_RETURN OP_ORACLE` coinbase outputs.
3. Reject more than one oracle output with `bad-oracle-multiple-outputs`.
4. If no oracle output exists, accept ordinary non-DD and DD transfer-only
   blocks; reject mint/redeem blocks with `bad-oracle-missing`.
5. Extract the bundle. Legacy v0x01/v0x02, unknown versions, truncated data,
   and malformed pushes fail extraction and reject with `bad-oracle-malformed`.
6. Require `bundle.IsMuSig2()`.
7. Run `ValidateMuSig2Bundle()`:
   - MuSig2 roster active at the block height
   - price in `MIN/MAX_ORACLE_PRICE_MICRO_USD`
   - aggregate signature is exactly 64 bytes
   - bitmap is non-empty and decodes
   - payload epoch matches block epoch
   - signer count meets `nOracleConsensusRequired`
   - every signer is inside `nOraclePubkeyCount`
   - aggregate BIP-340 signature verifies against the chain-bound message hash
8. Enforce timestamp freshness: not older than one hour and not more than
   60 seconds in the future relative to block time.

## Mempool And Price Source

DD transactions require a recent valid MuSig2 quote before mempool acceptance.
Ordinary DGB transactions do not consult DigiDollar or oracle validation.

Block validation uses the price extracted from the block's validated v0x03
bundle. It does not fall back to mock prices, exchange fetchers, or process-local
oracle state during consensus validation.

## Price Cache

After a block connects, `ConnectBlock()` updates the oracle price cache from
the validated v0x03 bundle. The cache stores micro-USD prices by block height,
keeps the most recent 1,000 entries, and removes/replays entries during
disconnect, reorg, reindex, and IBD flows.

## Activation

DigiDollar uses buried activation heights. Block oracle validation follows
the candidate's DigiDollar activation status. The static oracle P2P height
also applies to network oracle messages.

| Network | Buried DigiDollar height | Static oracle P2P height | MuSig2 height |
|---------|--------------------------|-------------------------|---------------|
| Mainnet | 23,869,440 | 23,627,520 | 23,627,520 |
| Testnet26 | 600 | 600 | 600 |
| Default regtest | 0 | 650 | 0 |

On regtest, `-digidollaractivationheight=N` sets the buried DigiDollar and
static DD/oracle/MuSig2 heights together. Once DigiDollar is active, mint/redeem
blocks must satisfy the V1 oracle rules above; transfer-only and ordinary DGB
blocks may omit the coinbase oracle bundle.

Thaw Day is a separate shared height for the new DigiDollar validation rules.
Mainnet is scheduled at 24,490,000, estimated for November 1, 2026. Testnet26
is scheduled at 432,100, estimated for September 18–19, 2026. The heights trigger
activation. Signet is unsupported and remains unscheduled. Thaw Day does not
change the oracle signing quorum or the v0x03 bundle format. See
the [activation guide](DIGIDOLLAR_ACTIVATION_EXPLAINER.md) for tip and next-block
status and the limits of this development candidate.

## Regtest Mocking

Regtest exposes mock price RPCs for deterministic tests. The mock path is not a
production fallback for consensus validation. Functional tests use it to produce
signed regtest v0x03 quote blocks and then validate the same block rules that
mainnet/testnet use after activation.

## Rejection Matrix

| Block shape | Expected result |
|-------------|-----------------|
| Non-DD block, no oracle output | accepted |
| Non-DD block, one valid v0x03 oracle output | accepted |
| Non-DD block, malformed or legacy oracle output | rejected |
| Any block with two oracle outputs | `bad-oracle-multiple-outputs` |
| DD transfer-only block, no oracle output | accepted |
| DD mint/redeem block, no oracle output | `bad-oracle-missing` |
| DD mint/redeem block, raw v0x01/v0x02 oracle output | `bad-oracle-malformed` |
| DD mint/redeem block, malformed v0x03 output | `bad-oracle-malformed` |
| DD mint/redeem block, v0x03 below quorum or wrong signature | `bad-oracle-musig2` |
| DD mint/redeem block, valid v0x03 output | accepted |
| DD transfer-only block, valid v0x03 output | accepted |

## Code References

- `src/oracle/bundle_manager.cpp` - bundle extraction, v0x03 serialization,
  MuSig2 validation, price cache, block oracle validator.
- `src/primitives/oracle.h` - `COracleBundle` and v0x03 payload helpers.
- `src/validation.cpp` - mempool oracle quote gate, ConnectBlock call sites,
  price-cache update and rollback.
- `src/node/miner.cpp` - miner template DD filtering and oracle-bundle
  insertion.
- `src/net_processing.cpp` - oracle P2P handlers for `oracleprice`,
  `oracleconsns`, `oracleattest`, `oramusnonce`, `oramusigctx`,
  `oramusigpsig`, `oraclehb`, `getoracles`, and legacy `oraclebundle` drop
  behavior.
- `src/kernel/chainparams.cpp` - active oracle keys, oracle node metadata, quorum,
  and activation parameters.

## Tests

The V1 format and block rules are pinned by:

- `src/test/digidollar_oracle_musig2_tests.cpp`
- `src/test/digidollar_oracle_bundle_matrix_tests.cpp`
- `src/test/musig2_p2p_message_tests.cpp`
- `src/test/fuzz/oracle_bundle_version_reject.cpp`
- `src/test/fuzz/oracle_validate_block_data.cpp`
- `test/functional/digidollar_oracle_block_rules_relay.py`
- `test/functional/digidollar_oracle_bundle_reject_matrix.py`
- `test/functional/digidollar_getoracles_consensus_field.py`
- `test/functional/digidollar_wave20_oracle_p2p.py`
- `test/functional/digidollar_wave21_musig2_p2p_dos.py`

These tests prove that legacy bundles are rejected, price-dependent DD blocks
require a valid v0x03 bundle, transfer-only and non-DD blocks can omit oracle
data, stale relay messages are bounded, and malformed oracle payloads cannot
update consensus price state.
