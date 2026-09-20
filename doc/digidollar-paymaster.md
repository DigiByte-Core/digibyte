# DigiDollar Paymaster Network

The Paymaster Network lets a wallet pay confirmed DigiDollar (DD) while a
selected provider contributes the DGB inputs needed for the miner fee. The
result is an ordinary `DD_TX_TRANSFER`; Paymasters have no consensus privilege
and existing validators need no Paymaster configuration.

**Documentation baseline:** source commit `bd270044c1` on
`feature/digidollar-paymaster-v1`, reviewed 2026-09-11. This is the operator and
RPC guide. Developers should start with [PAYMASTER.md](../PAYMASTER.md) and the
[implementation reference](digidollar-paymaster-implementation.md).

The current design and 28 V1 acceptance criteria are in the
[design specification](../DIGIDOLLAR_PAYMASTER_NETWORK_PROPOSAL_EN.md).
The [implementation plan](../DIGIDOLLAR_PAYMASTER_PLAN.md) explains the work
packages; the [hardening plan](../DIGIDOLLAR_PAYMASTER_HARDENING_PLAN.md) maps
the counterparty protections. Verification evidence and outstanding release
acceptance are recorded in the
[release gate](digidollar-paymaster-release-gate.md).

## Privacy and safety model

Paymaster transport and data minimization improve privacy but do not guarantee
anonymity. The selected provider necessarily learns the final transaction and
the confirmed transaction remains publicly analyzable. Standard mode requires
BIP324 v2 without v1 fallback. High-privacy mode additionally requires an onion
provider and Tor stream isolation, allows one provider attempt, and has no
clearnet fallback.

The wallet never sends a private key or seed. Protocol V5 treats the remote
counterparty as potentially malicious. Each side reconstructs the complete
transaction locally and signs only after a wallet-local authorization manifest
has been revalidated immediately before signing. The client signs only its DD
inputs; the provider signs only its pool inputs; both require
`SIGHASH_DEFAULT`. Unknown PSBT fields, roles, outputs, or sighash modes fail
closed.

This protects an honest wallet from signing a different recipient, amount,
change output, fee, or input set merely because a modified counterparty asks it
to. It is not a formal proof and does not make the remote service trustworthy.
Public sponsorship is financially bounded but cannot be made Sybil-fair. A
provider can retain data it necessarily receives, and can later publish the
exact transaction that the client already authorized. Local wallet compromise,
malicious release artifacts, and key theft are outside this threat model.

### Local metadata and retention

Wallet encryption protects private-key use; it must not be interpreted as
encryption of every value in the wallet database. Paymaster restart and
recovery records can contain provider endpoints, canonical requests and
responses, authorization manifests, unsigned and signed PSBTs, final
transactions, timing information, and links between attempts. High-privacy
mode protects the network path and suppresses endpoint-bearing logs, but it
does not add field-level encryption to these durable wallet records.

Treat the wallet file, full-wallet backups, storage snapshots, and diagnostic
archives as sensitive metadata even when private keys are encrypted. Do not
attach them to public issue reports. The secret restricted-sponsorship
capability is deliberately removed before a quote request is persisted; only
its cryptographic binding is retained.

These records are retained while they can still be needed for exact retry,
budget accounting, conflict handling, or recovery. Once an observed final
transaction reaches the 240-block reorganization safety depth, reconciliation
replaces an eligible final session and its validated children with a compact
idempotency tombstone. The tombstone still retains the minimum fields needed
to prevent a conflicting reuse, including the request/session binding, final
state and transaction identifier, fee modes, and local payment-order flags.
Unconfirmed, ambiguous, conflicting, or unreadable records are not deleted
automatically. Stopping Paymaster, locking the wallet, or disabling the feature
does not erase durable records. Do not edit or delete Paymaster database keys
manually; there is intentionally no destructive reset RPC.

An unreadable input-reservation record remains conservatively reserved. If the
provider-pool safety record cannot be decoded, automatic wallet coin selection
offers no inputs instead of treating an unknown pool as empty. Preserve the
wallet and diagnose or replace the development Paymaster data; do not work
around this condition by editing individual database keys.

## Node requirements

Discovery, relay, and client support default to `-paymaster=1`; use
`-paymaster=0` to disable all three. Starting a client session or provider also
requires:

```ini
digidollar=1
paymaster=1
prune=0
txindex=1
v2transport=1
```

The Paymaster P2P handshake, directory exchange, announcements, and direct
session messages all remain inactive until the shared DigiDollar activation
gate is active at the current chain tip.

The wallet requires `txindex` to be fully synchronized before it creates a
quote or a new Paymaster signature. Discovery and relay remain available to
upgraded nodes that are not configured for client or provider operation.

Automatic transfers require the exact Paymaster protocol V5 Capacity feature.
Peers that implement only an earlier Paymaster flow are excluded; there is no
silent downgrade. BIP324 v2 is still mandatory for direct transport, but is not
a substitute for the signed Provider Capacity proof.

Provider configuration and runtime state are wallet scoped. The normal first
start remains an explicit operator action. After that start, the recommended
`automatic` mode services the bounded provider queues without an operator click
for every message. This does **not** authorize a client signature: the client
still confirms the exact provider, model, recipient amount, and service fee
before its wallet signs. Optional provider autostart is a separate opt-in and
defaults to off. A provider wallet must be a descriptor wallet with local
private keys; legacy, watch-only, and external-signer wallets are not eligible.
`-paymasterendpoint=<host:port>` sets the announced endpoint. Loopback endpoints
are accepted only on regtest.

## Client use

Existing five-argument `senddigidollar` calls are unchanged and continue to use
the wallet's own DGB. Paymaster use is selected by the optional seventh `options`
object, after upstream `amount_unit` (`"cents"` or `"dollars"`). The older
Paymaster development branch used argument six for options; positional callers
must insert `"cents"` before that object when upgrading. Named `options` calls
remain unambiguous. Consult `help senddigidollar` for the exact argument schema used by the
running binary.

Example options object:

```json
{
  "fee_mode": "auto",
  "request_id": "550e8400-e29b-41d4-a716-446655440101",
  "maximum_paymaster_fee_cents": 5,
  "privacy": "standard",
  "selection": "lowest_total_cost"
}
```

Before the first Paymaster transfer, configure finite wallet-local service-fee
limits. The RPC limit in `senddigidollar` can lower, but never raise, these
wallet limits:

```text
setpaymasterclientsafetypolicy {
  "maximum_service_fee_per_transaction_cents": 100,
  "maximum_service_fee_per_day_cents": 1000
}
getpaymasterclientsafetystatus
```

Both limits must be positive. The effective per-transfer maximum is the minimum
of the wallet policy, the request's `maximum_paymaster_fee_cents`, and the
signed offer. Daily usage and active fee reservations are durable across
restart and use a monotonic accounting-time high-water mark.

- `fee_mode=dgb` always uses direct DGB funding.
- `fee_mode=paymaster` requires a Paymaster.
- `fee_mode=auto` may fall back only when the direct funding preflight reports
  insufficient DGB fee inputs. Wallet-lock and other errors do not trigger a
  fallback.
- `request_id` is a canonical lowercase UUID and is the durable idempotency key.
  Retrying the same request resumes the same session rather than creating a
  second payment.
- `maximum_paymaster_fee_cents` is a hard service-fee cap.
- `privacy` is `standard` or `high`; `selection` is `lowest_total_cost` or
  `privacy_weighted`.

### Prepare and authorize a Paymaster payment

For a Paymaster path, every `senddigidollar` call without an
`authorization_commitment` prepares the payment without creating a new user
signature. This remains true when `prepare_only` is omitted or false. A client
application must explicitly accept the exact returned commitment before Core
may sign. `prepare_only=true` and `authorization_commitment` are mutually
exclusive.

After configuring client safety limits, prepare a 10.00-DD recipient payment
using an integer amount of 1,000 cents:

```text
senddigidollar <address> 1000 "" 0 null "cents" {
  "fee_mode": "paymaster",
  "request_id": "550e8400-e29b-41d4-a716-446655440120",
  "maximum_paymaster_fee_cents": 100
}
```

Advance or inspect the same session as indicated by Core until its exact
authorization is ready. Review the returned provider, funding model,
recipient amount, fee and maximum outflow. To approve, repeat the identical
request with the returned commitment:

```text
senddigidollar <same-address> 1000 "" 0 null "cents" {
  "fee_mode": "paymaster",
  "request_id": "550e8400-e29b-41d4-a716-446655440120",
  "maximum_paymaster_fee_cents": 100,
  "authorization_commitment": "<exact-returned-hex-commitment>"
}
```

The placeholders must be replaced by the original recipient and exact returned
commitment. A mismatch is rejected. A locked wallet may still require unlocking
the same session. Inspect `authorization_required`, `authorization_accepted`,
session state and allowed actions; a prepared or authorized request is not yet
a confirmed payment. The examples are RPC-console notation; JSON-RPC clients
should pass structured parameters and shell users must quote the JSON for
their shell. Integer `amount` values are cents; values containing a decimal
point are interpreted as DD dollars by this RPC.

### Exact total outflow and emptying a DD wallet

By default, `amount` is the amount delivered to the recipient and a user-paid
Paymaster service fee is additional. Set
`subtract_paymaster_fee_from_amount=true` to instead treat `amount` as the exact
maximum DD outflow. Core derives an exact recipient amount for each eligible
offer so that:

```text
recipient amount + rounded Paymaster service fee = requested total outflow
```

For example, a total outflow of 5,000 cents with a 50-basis-point offer becomes
4,975 cents to the recipient plus a 25-cent service fee. A sponsored offer
delivers all 5,000 cents. The existing cent-rounding rule is unchanged; when no
exact split exists, Core returns `PAYMASTER_NO_EXACT_GROSS_OFFER` before it
reserves inputs or requests a signature.

`send_all_spendable_dd=true` additionally binds the request to every confirmed,
ordinary spendable DD input in the wallet. It requires the subtract option,
cannot be combined with manually selected inputs, and excludes unconfirmed,
reserved, and provider-pool DD. If that spendable snapshot changes before the
reservation is committed, Core stops with `PAYMASTER_SWEEP_BALANCE_CHANGED`.

Preparation example for an exact 50.00 DD wallet sweep; the same explicit
authorization step above is required before signing:

```text
senddigidollar <address> 5000 "" 0 null "cents" {
  "fee_mode": "paymaster",
  "request_id": "550e8400-e29b-41d4-a716-446655440110",
  "maximum_paymaster_fee_cents": 100,
  "subtract_paymaster_fee_from_amount": true,
  "send_all_spendable_dd": true
}
```

`getpaymasteroffers 5000
{"subtract_paymaster_fee_from_amount":true}` previews the exact recipient,
service-fee, and total values. With `fee_mode=auto`, successful direct DGB
funding still delivers the full entered amount; subtraction applies only if the
wallet actually falls back to a Paymaster. The lower-level
`requestpaymasterquote` continues to receive the already-derived recipient
amount.

Useful client inspection and recovery RPCs:

```text
listpaymasters
getpaymasteroffers
getdigidollarsendsession
listdigidollarsendsessions
resolvepaymastersession
getpaymasterreputation
clearpaymasterreputation
setpaymasterclientsafetypolicy
getpaymasterclientsafetystatus
```

`resolvepaymastersession` permits ordinary provider `fallback` only before a
user PSBT exists; `abandon_unsigned` additionally requires that no transaction
authorization can exist. Use the current Core-derived `allowed_actions`, not
the age of a quote, to determine which mutations are permitted.

After the user signature, timeout never releases the reserved inputs. An
ambiguous session must be checked again, retried with its exact provider and
artifacts, or recovered with `cancel_to_self`. A cancellation is not final when
merely accepted to the mempool; it becomes final only after confirmation.

## Capacity and signing firewalls

Relayed admission reserves are checked against both the active UTXO view and
the current mempool; an outpoint already used by a mempool transaction is not
advertised as available capacity. Directory replies copy at most the requested
bounded subset and rotate their starting point so a directory larger than one
reply is not permanently truncated to the same providers.

Before sending a payment intent, user DD outpoints, or a restricted capability,
the client sends `PMCAPREQ` and validates `PMCAPRESP`. The response binds the
genesis, provider and request IDs, client nonce, reference block, expiry, and
one exact operational resource set. The client verifies the provider identity
signature, every BIP86 control proof, creating transaction, chainstate outpoint,
value, and output script. The resulting `ValidatedCapacitySnapshot` is
persisted and the quote must use the same DGB and carrier outpoints.

Conflicting signed Capacity proofs or quotes are retained as local
equivocation evidence and block that provider locally. A proof cannot be reused
for contradictory local sessions. Exact protocol retries are idempotent;
reusing the same semantic message identity with different content is rejected,
even when it arrives over another peer connection.

The isolated channel is directional: the outbound client half sends only
Capacity, quote, submit, and recovery requests, while the accepted provider
half sends only their corresponding responses. Both send and receive paths
enforce that classification before a payload can enter the shared inbox.

Before an inbox message is acknowledged or subjected to the deeper chainstate
or PSBT firewall, the first valid identity-signed Capacity or quote claim is
atomically staged in the exact attempt or recovery record as evidence only. A
claim candidate grants no spending authority. An exact retry remains
idempotent after expiry, while a different valid signed claim creates durable
equivocation evidence. Database and persisted-local-authority failures leave
the message unacknowledged; only an unambiguously invalid remote artifact is
discarded.

The accepted quote produces a `ClientAuthorizationManifest` that binds the
original local request and its canonical hash, requested fee mode, privacy and
selection modes, provider, offer, policy, Capacity snapshot, exact DD inputs,
recipient, amount, wallet-owned change, service-fee ceiling, expiry, and
transaction template. The provider independently creates a
`ProviderAuthorizationManifest` binding the validated intent and input-control
proofs, its locally reserved pool outpoints, wallet-owned carrier/change
scripts, DD service fee, exact DGB network fee, safety reservation, and the same
template. Manifest V2 names the reservation's exact commit key and its maximum
reserved DGB network fee. The quote attempt and that ledger row are committed
atomically; a missing or different row is never reconstructed by a retry. A
new provider signature additionally requires the currently configured safety
policy hash. A later policy change cannot strand an already durable exact
commit, whose historical non-null policy hash and SPENT ledger binding remain
verifiable. RPC, Qt, background processing, retry, and recovery all call these
same core validators.

Before a client records a terminal result, Core verifies the complete raw
transaction, non-witness transaction, txid, wtxid, every input/output and every
witness against the trusted PSBT template. All signatures are checked with the
actual prevouts by the script interpreter. If the transaction is not already
confirmed or in the mempool, a mempool preflight must succeed. An invalid
witness or conflict remains recoverable and cannot overwrite a previous
`FINAL_COMMITTED` state. Result, attempt, and session transitions are stored
atomically.

### Qt client fee selection

The **Send $DD** page defaults to **Pay with my DGB**. This safe default never
selects a Paymaster or adds a DD service fee. **Automatic** first performs the
ordinary DGB funding preflight and considers a Paymaster only when suitable DGB
fee inputs are insufficient; other errors never cause a fallback. **Use a
Paymaster** requires an eligible provider.

The normal view explains who funds the DigiByte network fee and summarizes the
recipient amount and maximum wallet outflow. Paymaster offer inspection,
provider-attempt limits, privacy profile, and provider selection are collapsed
under **Advanced Paymaster settings**. Mouse-wheel input does not change those
spin boxes or combo boxes. User-paid offers charge the displayed DD service
fee, while sponsored offers charge no DD service fee. The exact provider,
funding model, recipient amount, fee, and authorization commitment are still
confirmed before signing.

In Automatic and Paymaster modes, **Deduct the Paymaster fee from the entered
amount** makes the entered value the exact total outflow. **Empty wallet with
Paymaster** uses the confirmed ordinary spendable DD balance and enables that
exact mode. Qt separately displays total outflow, recipient amount, provider
fee, and remaining spendable balance; the option is off for every new transfer
and is absent from direct-DGB mode.

Automatic and Paymaster modes require a positive wallet-local client safety
policy. Qt reads the existing `getpaymasterclientsafetystatus` result and can
configure the same policy directly from the send page; this does not expose or
weaken the backend limits. A running or ambiguous collaborative transfer is
shown separately from fee selection so retry, fallback, and same-input recovery
remain visible without presenting them as ordinary fee settings.

Qt treats Core as authoritative for both the durable session snapshot and its
allowed recovery actions. It never treats the generic `final` field alone as a
successful payment, never combines a refreshed session with an older attempt or
artifact, and never resumes signing automatically after a wallet or process
restart. `listdigidollarsendsessions` supplies a bounded wallet-local inbox for
unfinished or recovery-relevant sessions; Qt refreshes the selected session
before enabling an action.

## Provider setup

In Qt, the provider-only **Paymaster Network** tab is hidden by default. Enable
**Show Paymaster operator controls** under **Settings → Options → Wallet →
Expert** to configure or operate a provider. This display preference does not
disable Paymaster client payments or discovery.

On the first visit for a wallet, Qt asks the operator to choose **Guided setup**
or **Manual expert setup**. Configuration, Safety limits, and Liquidity remain
locked until that choice is completed. Activity and recovery remains available
so a local onboarding preference can never hide durable reservations or
recovery work. Existing provider identities, policies, pool entries, or runtime
state are detected and unlock the console automatically. The choice is stored
as a wallet-scoped Qt preference; Core remains authoritative for readiness.

The guided setup uses a theme-controlled classic dialog on Windows. It covers
current prerequisites, explicit provider-wallet selection, public identity,
user-paid/public-sponsored/restricted service models, offer limits, a finite
safety profile, policy-aware liquidity targets, and a final review. A dedicated
provider wallet is recommended, but not required. Because the wizard belongs
to the currently selected wallet view, it never silently creates or switches a
wallet: choosing another wallet closes the assistant and directs the operator
to Core's normal wallet menu before reopening Paymaster Network. The selected
wallet name is pinned throughout the review and Core rechecks descriptor,
private-key, and external-signer eligibility immediately before the first
write. The assistant opens only after provider, safety, and liquidity status
have all been read successfully. On later runs it imports the authoritative
wallet-persisted values instead of replacing them with setup defaults; every
editable page also offers an explicit **Restore defaults** action.

Applying the reviewed plan first places an existing enabled provider safely
offline. It then creates or reuses the identity, writes a temporary compatible
safety bridge when an operating-policy transition requires one, saves the new
operating and final safety policies, saves liquidity targets and finite
maintenance limits, and rechecks the pool. Only still-missing outputs are
created, and only after a second confirmation of the exact current funding
plan. Runtime mode, autostart, and the requested enabled state are persisted
last. Existing identity and pool outputs are retained. Completed write steps
remain durable and retryable after a later failure; closing before the final
Apply confirmation writes nothing. The completion page confirms that no
additional Save buttons are required.

A fresh guided setup defaults to the recommended `automatic` operation mode,
`autostart=false`, and an enabled provider configuration. Reopening the
assistant preserves existing runtime, autostart, and enabled choices. Therefore
an explicitly selected enabled/autostart combination may start after setup when
all readiness gates pass; otherwise starting remains an explicit Overview
action. Disabling a provider configuration always prevents a saved autostart
preference from bringing it online. Paid automatic maintenance is separately
disclosed and approved because it may create DGB-fee transactions. The
assistant remains available from Overview for later review.
Manual queue processing and provider autostart are advanced operator settings.
Manual expert setup exposes all detailed controls after an explicit risk
warning.

When the assistant creates a new provider identity, its completion page shows
the full provider ID and asks for a new full-wallet backup. The identity private
key, policies, pool records, durable sessions and provider finance ledger are
wallet metadata; a seed phrase or descriptor export alone is not a complete
Paymaster-provider recovery. The reminder is deliberately non-blocking, but it
remains visible on Overview and Finances until `backupwallet` succeeds or the
operator explicitly acknowledges an external full-wallet backup procedure.
Material offer, safety or liquidity-policy changes recommend another backup;
ordinary payments and finance bookings do not produce repeated reminders.

### Provider runtime modes

Provider runtime preferences are persisted in the provider wallet:

- `operation_mode=automatic` is the default. Once the wallet-scoped provider
  runtime has been started, Core periodically handles bounded Capacity, quote,
  submitted-payment, and recovery work. Each scheduler pass performs at most
  one request-processing step and one submit-processing step. It uses the same
  validation, authorization manifests, finite budgets, replay protection, and
  atomic wallet writes as the expert RPC path.
- `operation_mode=manual` is an expert mode. Core keeps the provider reachable,
  but the operator explicitly invokes the processing RPCs. A mode change is
  accepted only while the provider is stopped. The stopped-state check and
  runtime-settings commit share the wallet-local provider-work guard with
  start/autostart, so a concurrent start cannot overtake the mode change.
- `autostart=false` is the default for current-format provider settings.
  Enabling autostart permits an already configured wallet to start its provider
  runtime when readiness permits after load. Older settings records are rejected
  rather than migrated or overwritten. No passphrase is stored.
- The low-level `stoppaymaster` RPC serializes with a provider start and returns
  `PAYMASTER_PROVIDER_BUSY` instead of falsely reporting success while another
  wallet-local provider operation owns the transition. It remains an ephemeral
  stop when autostart is saved as enabled.
- The Qt **Stop** action is persistent: when autostart is enabled it first
  saves `autostart=false` and only then stops the runtime. The separate
  **Disable provider configuration** action persists `enabled=false` and also
  stops the runtime, so neither scheduler polling nor a wallet reload can undo
  the operator's intent.

An encrypted, locked provider wallet does not consume queued work or create new
signatures. The automatic service reports `waiting_for_unlock` and continues
after a later wallet unlock. With autostart enabled, other failed readiness
gates report `waiting_for_readiness`; they do not authorize spending.
Drain-only operation continues only already durable signed or committed work
and does not create new Capacity proofs or quotes.

The provider lifecycle is deliberately staged:

1. Create or load an eligible descriptor wallet and unlock it.
2. Create its wallet-managed BIP86 identity with
   `createpaymasteridentity`.
   Immediately create a full-wallet backup. Restoring that file preserves the
   same provider ID and its wallet-local metadata; creating a new wallet starts
   a new identity and a separate finance history.
3. While the provider is stopped, set a policy with `setpaymasterpolicy`. The
   RPC holds the wallet-scoped provider-work guard across the stopped-state
   check and database commit, so autostart cannot race the policy change. The absolute
   `maximum_network_fee_dgb_satoshis` must be positive.
   A public provider may advertise `user_paid` and `sponsored` at the same
   time. User-paid transfers use the configured DD service-fee rate, while
   sponsored transfers charge no DD service fee. Restricted sponsorship is a
   sponsored-only invitation mode and cannot be combined with `user_paid`.
4. Set a complete finite provider loss/rate policy with
   `setpaymastersafetypolicy` and inspect it with
   `getpaymastersafetystatus`. A funding model that the provider advertises
   requires six positive limits; six zero values disable only an unadvertised
   model and never mean unlimited. The setter shares the wallet-scoped
   provider-work guard with automatic servicing, so a scheduler step cannot
   reserve work against the old limits while their replacement is committed.
   The Qt Safety limits page explains these wallet-local spending and request
   controls. Its reset buttons change only the unsaved form: user-paid limits
   return to finite recommended starting values, unused sponsored models return
   to safe all-zero disabled values, and quote/client sections return to their
   recommended starting values. Saving is always required before a reset takes
   effect in the wallet backend. Qt only assembles and displays these values;
   the Core RPC remains solely responsible for validating every safety-policy
   relationship and rejecting an invalid configuration.
5. Enable the wallet using `setpaymasterenabled true`.
6. Preview and explicitly execute `preparepaymasterpool`, passing the returned
   `plan_id` unchanged with `execute=true`. Admission requires at
   least three independent confirmed DGB slots of at least 10,000,000 sat each.
   A `USER_PAID` policy additionally requires at least three confirmed admission
   DD carriers. Admission and operational pool entries are separate.
   The Qt Liquidity page explains the four target classes and guides a new
   operator through restore/choose targets, preview, review, execution, and
   confirmation. Restoring targets never moves funds. Execution is enabled only
   for the unchanged target set and cryptographic plan from the matching latest
   preview; Core recomputes that plan before unlocking or constructing a
   transaction. `rebalancepaymasterpool` uses the same binding for the exact
   entries being retired. Manual pool calls also share the provider-work guard
   with automatic maintenance. Operational DGB entries below the current
   advertised network-fee ceiling do not satisfy a target, and rebalancing
   retains higher-value usable entries before retiring undersized ones. The default
   targets are three admission and one operational DGB slot, plus three
   admission and one operational DD carrier when `USER_PAID` is selected;
   sponsored-only defaults use no carriers.
7. Save the ongoing targets and finite maintenance budget with
   `setpaymasterliquiditypolicy`, then inspect them with
   `getpaymasterliquiditystatus`. Automatic replenishment does not make a zero
   budget unlimited: paid maintenance remains disabled until the operator sets
   `paid_maintenance_approved=true` together with positive per-transaction,
   rolling-hour, and rolling-day fee limits.
8. Keep the recommended automatic runtime, or while stopped use
   `setpaymasterruntimesettings` to select manual expert operation and/or opt in
   to autostart. Paymaster settings must use the current persisted format;
   older development records are not migrated and must be replaced together
   with the development Paymaster data before provider operation can continue.
9. Inspect `getpaymasterinfo`, `getpaymasterpoolinfo`,
   `getpaymastersafetystatus`, `getpaymasterliquiditystatus`, and
   `getpaymasterfinancestatus`, review the effective finite budgets, and
   explicitly call `startpaymaster`. Its response
   echoes the effective safety policy used for that start. A provider whose
   only failed readiness gates are missing pool slots can start in a maintenance
   wait state; it does not advertise or accept new work until the configured
   targets are confirmed.
   In automatic mode Core then services the bounded queues. In manual mode,
   `processpaymasterrequests` handles at most one capacity, quote, or
   recovery-provider request and `processpaymastersubmits` handles at most one
   client-signed payment or recovery submission. These manual RPCs reject while
   automatic servicing is active with `PAYMASTER_AUTOMATIC_SERVICE_ACTIVE`.
   Both paths can sign only provider-owned inputs for an exact transaction that
   passes the existing provider authorization manifest and safety policy.
10. Use `stoppaymaster` before planned maintenance or changing operation mode.
   Configuration and durable commits remain in the wallet database.

`getpaymasterinfo` includes `settings_present` so an identity-only wallet can
be distinguished from a deliberately disabled persisted provider
configuration. Qt uses this distinction when deciding whether a reopened
wizard should preserve `enabled=false` or apply the fresh-setup default.

Provider RPCs include:

```text
createpaymasteridentity
setpaymasterpolicy
setpaymastersafetypolicy
getpaymastersafetystatus
setpaymasterliquiditypolicy
getpaymasterliquiditystatus
withdrawpaymastercarrier
getpaymasterfinancestatus
acknowledgepaymasterproviderbackup
setpaymasterenabled
setpaymasterruntimesettings
preparepaymasterpool
rebalancepaymasterpool
getpaymasterinfo
getpaymasterpoolinfo
listpaymasterreservations
cancelpaymasterquote
processpaymasterrequests
processpaymastersubmits
startpaymaster
stoppaymaster
```

### Provider finances and backup

The Qt **Finances** page is an operator dashboard, not a consensus or tax
accounting authority. Its wallet-local ledger is bound to the active chain's
genesis hash and exactly one provider ID. Ledgers belonging to different
provider identities are never merged automatically. It records:

- confirmed user-paid service fees as native DD income;
- the actual native DGB network fee of user-paid, public-sponsored and
  restricted-sponsored transfers;
- actual confirmed DGB fees for pool setup, replenishment, retirement and
  carrier-excess withdrawal; and
- pending transactions separately until confirmation.

Prepared pool value remains wallet-owned operating capital, not an expense.
The dashboard therefore reports available, reserved and pending DGB capacity,
the 1.00-DD principal of each active carrier, and carrier fee surplus separately
from income and costs. Replays are idempotent, and confirmation, reorg or
conflict changes rebuild the UTC daily totals instead of incrementing them a
second time.

`getpaymasterfinancestatus` accepts `today`, `7d`, `30d` or `all`, and can
optionally return bounded event pages with cursor pagination. Qt requests 250
rows per page; a complete CSV export loads every page asynchronously and writes
the destination atomically, while the interactive table remains bounded. The
detailed form also returns the latest 366 materialized UTC-day totals for the
selected period so operator interfaces do not need to load or expose transaction
IDs to draw the daily progression. Its selected-period model breakdown separates
transfer counts, DD income and DGB transfer costs for user-paid, public
sponsored and restricted sponsored operation. Native DD and DGB values are
authoritative. When a
current Oracle price exists, the RPC and Qt may show a current-price USD
estimate and its valuation time; no historical exchange rate is invented.
Accounting reconciliation derives an event only from current-format durable
wallet records that still prove its exact native amounts. It never upgrades an
older record format or estimates missing amounts; the result marks any earlier
unprovable history as partial.

The Finances page links to the existing preview-first carrier-withdrawal,
carrier-release, DGB-retirement and liquidity-preparation controls. Preview and
execution remain distinct and retain the provider-stop, plan-ID, reservation
and finite-budget checks described below. Its CSV export contains only the
selected accounting period and native amounts (plus an optional current-price
valuation); it intentionally excludes keys, recipients, network identity data
and raw transactions. A CSV file is not a wallet backup.

`backupwallet` is the supported complete provider backup. After the backup file
has been created, the source wallet records only the completion time, never the
destination path. Restoring the copied file intentionally presents the reminder
again so the restored installation can create its own current backup. Operators
using another full-wallet backup mechanism may call:

```text
acknowledgepaymasterproviderbackup {"external_backup":true}
```

That acknowledgement does not create or validate a backup and must never be
used for seed-only or descriptor-only exports. Existing downgrade protection
through `WALLET_FLAG_PAYMASTER_AUTHORIZATION` remains unchanged.

A complete backup necessarily includes the durable Paymaster metadata and
signed artifacts described above. Wallet key encryption alone does not imply
field-level encryption of that metadata. Store and transport backup files with
access controls appropriate for transaction history and provider-relationship
data, keep only the required generations, and securely retire obsolete copies
according to the operator's backup policy. Never use CSV finance export as a
substitute for this backup or as a source for recovery.

A backup preserves the off-chain Paymaster records available when it was
created. A blockchain rescan does not reconstruct later signed provider results,
sponsorship-use records, or the complete provider budget/finance ledger. An older
client backup with an authorized payment may therefore reject a retry with
`PAYMASTER_INVALID_CAPACITY_DGB_CHAINSTATE` when its provider inputs are already
spent, even though the wallet rescan finds the confirmed payment. This rejection
must not be bypassed by starting a new payment with a new request ID.

After restoring an older image, verify the original payment on chain and reconcile
missing off-chain records from the newest complete backup before resuming service.
Start the recovery node with `-paymaster=0` before loading the restored wallet,
then persist `setpaymasterruntimesettings {"autostart":false}` for that wallet.
Keep provider autostart disabled during reconciliation and never operate two
loaded copies of the same provider wallet. The older-client-backup regression
test does not establish provider-budget or sponsorship anti-rollback guarantees
for stale backups. See the [remaining security checks](digidollar-paymaster-release-gate.md#additional-security-boundary-checks-2026-09-19).

`setpaymasterruntimesettings` accepts an object containing
`operation_mode` (`automatic` or `manual`) and/or `autostart` (boolean).

```text
setpaymasterruntimesettings {"operation_mode":"automatic","autostart":false}
```

`getpaymasterinfo` and `startpaymaster` report `operation_mode`, `autostart`,
and `service_state`. In addition to `stopped`, `waiting_for_unlock`,
`waiting_for_readiness`, `active`, `manual`, `drain_only`, and `error`, the
service may report `waiting_for_maintenance_approval`,
`replenishing_liquidity`, or `waiting_for_liquidity_confirmation`.
`last_service_error`, when present, is a stable, privacy-neutral diagnostic.
`getpaymasterinfo.liquidity` contains the same target, pending, missing,
maintenance-budget, and carrier-value summary returned by
`getpaymasterliquiditystatus`. `startpaymaster` also returns the effective
finite `safety_policy` used for the operator's start review.

Pool preparation and rebalancing are transaction-creating operations only when
their explicit execution option is set. Preview the returned plan before
executing it. Locking the wallet pauses automatic queue processing before
messages are consumed and prevents new quotes and signatures; it does not erase
configuration or prevent later recovery of an already durable final commit.

### Automatic liquidity lifecycle

Operational inputs are not treated as disposable after a successful payment.
When the provider atomically commits the fully validated final transaction,
Core also records its exact wallet-owned replacement outputs from the provider
authorization manifest:

- A returned carrier becomes a `PENDING_SUCCESSOR` containing the previous
  carrier principal plus the user-paid service fee. A 1.00 DD carrier that
  earns 0.03 DD therefore becomes a 1.03 DD operational carrier, and can later
  become 1.06 DD without a separate carrier-maintenance transaction.
- Wallet-owned DGB change becomes a `PENDING_SUCCESSOR` only when its value is
  still at least the provider's required per-transfer DGB capacity. Smaller
  change remains ordinary wallet value and the missing target may require paid
  replenishment.

The successor provenance is persisted with the provider commit or maintenance
operation that created it. After the required confirmation it becomes
`AVAILABLE`; a reorg moves it back to `PENDING_SUCCESSOR`, and a conflicting or
otherwise unusable output becomes `INVALIDATED`. Startup reconciliation is
idempotent and can reconstruct still-unspent successors from previously
confirmed current-format provider commits. Only active pool states protect
funds from ordinary wallet coin selection; `RELEASED` and `INVALIDATED` entries
do not.

`ProviderLiquidityPolicy` is wallet local and controls:

- automatic replenishment;
- admission and operational targets for DGB and DD carriers; and
- finite maximum maintenance fees per transaction, rolling hour, and rolling
  day.

The status RPC reports each class as `ready`, `pending`,
`counted_toward_target`, and `missing`. `AVAILABLE`, `RESERVED`, and
`PENDING_SUCCESSOR` count toward a target so a scheduler retry cannot create a
duplicate replacement, but only confirmed `AVAILABLE` entries are ready for a
new request. A restartable maintenance ledger stores the exact planned outputs,
transaction, reserved fee, actual fee, and monotonic accounting time.

In a started automatic provider, Core first reconciles free successors and any
already durable maintenance transaction. It then creates at most the missing
liquidity, waits for confirmation, and only then resumes announcements and new
requests. Already authorized submissions can still be completed safely. A
locked wallet, insufficient DGB, disabled automatic replenishment, or exhausted
maintenance budget pauses new work without consuming queued requests. A
stopped provider and a provider in manual mode never create paid maintenance
transactions automatically.

Existing providers receive suggested targets of at least three admission and
one operational DGB slot, plus corresponding carrier targets when `USER_PAID`
is enabled. Higher existing active counts are retained. Suggested maintenance
ceilings are the lowest positive ceilings of the active provider safety
classes. The operator must approve these finite limits once before the first
paid replenishment; free successor recycling does not spend the maintenance
budget.

### Carrier excess and slot release

Carrier service fees remain wallet-owned but stay protected while their output
is an active pool slot. `withdrawpaymastercarrier` is preview-first and offers
two distinct operations:

```text
withdrawpaymastercarrier {"mode":"all_excess"}
withdrawpaymastercarrier {"mode":"all_excess","execute":true,"plan_id":"PLAN"}

withdrawpaymastercarrier {"mode":"release_slot","txid":"TXID","vout":0}
withdrawpaymastercarrier {"mode":"release_slot","execute":true,"plan_id":"PLAN"}
```

- `all_excess` selects only confirmed, available carriers with more than 1.00
  DD. It creates fresh wallet-owned replacement carriers of exactly 1.00 DD and
  combines only the excess into a fresh wallet-owned DD output. This is a DD
  transaction, requires ordinary non-pool DGB fee inputs, and charges its DGB
  fee to the finite maintenance budget.
- `release_slot` marks one confirmed, available operational carrier as
  `RELEASED` and atomically reduces the operational-carrier target by one. It
  is wallet local and costs no network fee, but the provider must be stopped.

Execution requires the unchanged, unexpired `plan_id` from the immediately
preceding preview. Reserved, unconfirmed, authorized, spent, or invalidated
carriers cannot be withdrawn. Releasing the last carrier leaves `USER_PAID`
configured but not ready until liquidity is restored.

A zero operational-carrier target is therefore a valid, intentional stopped
state, not a repair request. `getpaymasterinfo` reports
`PAYMASTER_LIQUIDITY_TARGETS_INCOMPLETE` and
`getpaymasterliquiditystatus` reports `waiting_for_target_configuration` until
the operator explicitly raises and saves the carrier target. Provider start and
autostart must not claim that automatic maintenance can repair this state:
maintenance only restores missing outputs *up to the saved targets*.

Automatic-liquidity regression coverage is intentionally split by failure
surface. Provider unit tests cover target-policy coherence, explicit approval,
per-transaction and rolling maintenance ceilings, replay conflicts, release,
and monotonic accounting. Wallet-store tests cover atomic successor writes,
database failures, restart reconstruction, confirmation, reorg, conflict, and
idempotency. Functional tests exercise free carrier/DGB recycling and paid DGB
replacement after real transfers, plus paid carrier replacement with disabled
automation, missing approval, pending-confirmation restart, duplicate-scheduler
ticks, confirmation promotion, and exhausted rolling budget. The
`paymaster_pool_lifecycle` stateful fuzz target permutes pool reservations,
commits, successors, maintenance records, restarts, reorgs, conflicts, and
withdrawal plans while continuously checking durable invariants. No single
happy-path test is treated as proof of the lifecycle.

The provider safety policy has separate classes for `user_paid`,
`public_sponsored`, and `restricted_sponsored`. Each class limits the network
fee per transaction, simultaneously reserved fee, rolling-hour and rolling-day
fees, and completed transactions per hour/day. The policy also limits active
quotes globally, per netgroup, and per wallet-HMAC-pseudonymized recipient, plus
quote requests per netgroup/minute. Raw IP addresses are not persisted as
accounting keys.

The maximum possible DGB network fee is atomically reserved with a quote,
converted to durable spend by final commit, and released only by a safe
unsigned abort. Limits, reservations, usage, and accounting time survive
restart. When a class reaches a limit it returns
`PAYMASTER_SAFETY_LIMIT_EXHAUSTED`, stops creating quotes and announcements for
that class, and never interprets a missing/zero limit as unlimited.

A provider with no readiness for new work may enter drain-only operation when
durable signed or committed work exists. It publishes no new capacity and
accepts no new quotes, but can finish exact authorized submits and recovery.
Locking the wallet similarly prevents new signatures without erasing durable
recovery state.

Direct message admission uses bounded token buckets per peer, netgroup,
provider, and session, with fair queuing between sessions. Cheap size, replay,
announcement, and rate checks run before expensive signature or chainstate
validation. Capacity, submit, capability, and result replay state that affects
safety is persisted. Expired unsigned quotes/capacity reservations are released
atomically; a user- or provider-signed authorization is never unlocked merely
because its original TTL elapsed.

Generation-bound, reference-counted RAII leases protect inbox messages across
the persistence decision. TTL pruning and both generic and specific dequeue
operations skip an active lease. An exception or failed write releases only
the lease and preserves the claim for retry; acknowledge/consume is the sole
terminal removal.

## Fee behavior

Provider percentage fees are limited to 10,000 basis points, in multiples of
10, and round upward to a whole DD cent:

- 0 cents: no provider DD fee output or carrier;
- 1–99 cents: a confirmed carrier input and valid successor are required;
- 100 cents or more: a normal provider DD output is used.

Public and restricted sponsorship charge exactly 0 DD service fee. Restricted
capabilities are bound to one concrete payment and are not globally gossiped.
Public sponsorship must still have explicit finite hourly and daily DGB budgets;
the budgets cap loss but do not provide Sybil fairness.

For restricted operation, configure a sponsored-only policy with
`sponsorship_scope=restricted`. `startpaymaster` starts the direct provider
runtime but deliberately publishes no announcement. An authenticated sponsor
service supplies its x-only authorization key to
`createrestrictedpaymasterdescriptor`; the returned descriptor binds that key,
the provider identity, endpoint, policy, and current admission reserve.

The sponsor signs a one-payment capability for the descriptor. The client
supplies `provider_identity_key`, `restricted_service_descriptor`, and
`sponsorship_capability` together in the seventh `senddigidollar` options object.
Restricted sessions use exactly one provider attempt. The capability plaintext
exists only in memory and on the encrypted direct connection: wallets persist
only its hash, payment binding, reservation, and consumed state. Do not pass a
capability on a command line or store it in shell history; use an authenticated
JSON-RPC client or protected application channel.

## Recovery with an independent provider

For an ambiguous authorization, `retry_same` may contact only the exact original
provider and artifacts. `cancel_to_self` can instead select a distinct
`USER_PAID` recovery provider through the `options` object accepted by
`resolvepaymastersession`. The recovery provider must differ from the original,
pass the full V5 Capacity handshake, and satisfy the inherited privacy profile.

The recovery transaction reuses the reserved user inputs but returns all DD to
fresh wallet-owned scripts. The only permitted deduction is the locally bounded
recovery service fee; all DGB fee inputs belong to the recovery provider. The
client first receives an exact recovery authorization commitment and must
explicitly resubmit that commitment before its inputs are signed. Original user
inputs remain reserved until either the original transaction or the recovery
transaction is confirmed.

Useful optional `cancel_to_self` fields are `recovery_provider_id`,
`recovery_offer_id`, `maximum_recovery_service_fee_cents`, `prepare_only`, and
`recovery_authorization_commitment`; use `help resolvepaymastersession` for the
running binary's exact schema. If neither the client has DGB nor a distinct
eligible recovery provider is reachable, delayed publication of the already
authorized original transaction remains an unavoidable on-chain risk.

## Diagnostics

Start with the authoritative RPC output rather than debug logs:

```text
getpaymasterinfo
getpaymasterpoolinfo
getpaymastersafetystatus
getpaymasterliquiditystatus
getpaymasterclientsafetystatus
listpaymasters
getdigidollarsendsession <request_id>
listdigidollarsendsessions
listpaymasterreservations
```

Production logs intentionally omit payment details, raw PSBTs, capabilities,
and key material. The provider functional test exercises this boundary with
verbose RPC and network logging and rejects known recipients, request/session
identifiers, descriptors, capabilities, PSBTs, recovery transactions, and key
material in either node's `debug.log`. Session responses expose authoritative
`session_state`, `pending_phase`, `final`, `broadcast_state`, and
`confirmation_state` fields.

SOCKS authentication values used for stream isolation are never logged.
Connections opened for high-privacy Paymaster operation also redact the target
host and port from proxy success and failure messages. This log protection does
not reduce the sensitivity of the wallet database or its backups.

All Paymaster clients require `-v2transport=1`; a direct Paymaster connection
never falls back to plaintext P2P v1. High privacy additionally requires an
onion endpoint, a configured onion proxy with `-proxyrandomize=1`,
`-logips=0`, `-capturemessages=0`, and exactly one provider attempt. These
requirements are checked before Core creates a payment session or reserves
wallet inputs, and are rechecked when the isolated connection is opened.

`-paymaster=0` disables announcement discovery/relay and rejects new client or
provider operation. Paymaster operation is also unavailable before DigiDollar
activation. Unloading a provider wallet immediately removes its ephemeral
running state. Loading it again restores its durable identity, policy, pools,
recovery records, and runtime preferences. It remains stopped by default;
runtime restart after load occurs only when the operator previously enabled
`autostart` and the wallet satisfies readiness (or waits for a later unlock).

## Automatic completion of a finite setup command

An executed `preparepaymasterpool` command persists a finite authorization and
can now complete later through the existing wallet scheduler, including while
provider service is stopped. This exception applies only to an explicitly
approved setup, not recurring paid maintenance. Inspect `accepted` separately
from `executed`, and monitor `getpaymasterpoolinfo.preparation`. The default fee
ceiling is 0.2 DGB per setup transaction, at most two transactions per command.
See [automatic pool setup](digidollar-paymaster-pool-setup.md) for waiting states,
unsigned cancellation, fee changes, and recovery of a previously saved but
unregistered DGB transaction.
