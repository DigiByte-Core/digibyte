#!/usr/bin/env python3
"""Rehearse the same DigiDollar operations before and after Thaw Day.

This follows test_multi_oracle_testnet.sh. It uses fresh local wallets and
real oracle signing, with controlled exchange responses for repeatable prices.
It never opens an existing wallet or connects to the public testnet.
"""

import argparse
import base64
from decimal import Decimal
import hashlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
from pathlib import Path
import signal
import socket
import subprocess
import sys
import time
import urllib.error
import urllib.parse
import urllib.request

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "test/functional"))
H = 5000
BASE_PRICE = 30000
NEW_PRICE = 45000
NAMES = ["bob", "alice", "charlie", "dave", "eve", "frank", "grace", "heidi"]
SLOTS = [[0, 1, 16, 18], [2, 3, 17, 19], [4, 5, 20], [6, 7, 21],
         [8, 9, 22], [10, 11, 23], [12, 13], [14, 15]]


class Failure(RuntimeError):
    pass


class RPCError(Failure):
    def __init__(self, error):
        self.code = error["code"]
        super().__init__(error["message"])


def save(path, value):
    temporary = path.with_suffix(path.suffix + ".new")
    temporary.write_text(json.dumps(value, indent=2, default=str) + "\n")
    temporary.replace(path)


def digest(path):
    with path.open("rb") as handle:
        return hashlib.file_digest(handle, "sha256").hexdigest()


def check(condition, message):
    if not condition:
        raise Failure(message)


def wait_for(fn, description, timeout=180):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if fn():
            return
        time.sleep(1)
    raise Failure("Timed out: " + description)


def free_port():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def process_identity(pid):
    try:
        # The start time distinguishes this process from a later reused PID.
        return Path(f"/proc/{pid}/stat").read_text().rsplit(")", 1)[1].split()[19]
    except FileNotFoundError:
        return None


def serve_feed(run):
    """Keep the real exchange parsers; supply only their HTTP responses."""
    state = json.loads((run / "run.json").read_text())

    class Handler(BaseHTTPRequestHandler):
        def log_message(self, *_args):
            pass

        def do_GET(self):
            route = urllib.parse.urlsplit(self.path)
            query = urllib.parse.parse_qs(route.query)
            price = Decimal((run / "price-micro-usd.txt").read_text().strip()) / 1000000
            text_price = format(price, ".6f")
            routes = {
                "/Binance/api/v3/ticker/price": {"price": text_price},
                "/CoinGecko/api/v3/simple/price": {"digibyte": {"usd": float(price)}},
                "/KuCoin/api/v1/market/orderbook/level1": {"data": {"price": text_price}},
                "/Gate.io/api/v4/spot/tickers": [{"last": text_price}],
                "/HTX/market/detail/merged": {"tick": {"close": float(price)}},
                "/Crypto.com/exchange/v1/public/get-tickers": {"result": {"data": [{"a": text_price}]}},
            }
            response = routes.get(route.path)
            if route.path.startswith("/Binance/") and query.get("symbol") != ["DGBUSDT"]:
                response = None
            body = json.dumps(response if response is not None else {"error": "Unknown test feed route"}).encode()
            self.send_response(200 if response is not None else 404)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            with (run / "feed-requests.jsonl").open("a") as log:
                log.write(json.dumps({"time": time.time(), "path": self.path,
                                      "price_micro_usd": int(price * 1000000),
                                      "served": response is not None}) + "\n")

    ThreadingHTTPServer(("127.0.0.1", state["feed_port"]), Handler).serve_forever()


class Node:
    def __init__(self, lab, index, record):
        self.lab, self.index, self.record = lab, index, record
        self.name = record["name"]
        self.directory = Path(record["datadir"])
        self.wallet = "thaw-test-" + self.name
        self.process = None

    def rpc(self, method, *params, wallet=False):
        cookie = self.directory / "testnet26" / ".cookie"
        authorization = base64.b64encode(cookie.read_bytes().strip()).decode()
        path = "/wallet/" + urllib.parse.quote(self.wallet) if wallet else "/"
        request = urllib.request.Request(
            f"http://127.0.0.1:{self.record['rpc_port']}{path}",
            json.dumps({"jsonrpc": "2.0", "id": "thawday", "method": method, "params": list(params)}).encode(),
            {"Authorization": "Basic " + authorization, "Content-Type": "application/json"})
        # Local RPC must not use an HTTP proxy inherited from the desktop.
        opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
        try:
            response = opener.open(request, timeout=300)
        except urllib.error.HTTPError as error:
            response = error
        data = json.loads(response.read(), parse_float=Decimal)
        if data.get("error"):
            raise RPCError(data["error"])
        return data["result"]

    def start(self, reindex=False):
        self.directory.mkdir(exist_ok=True)
        marker = self.directory / "THAW_DAY_TEST_ONLY"
        if not marker.exists():
            check(not list(self.directory.iterdir()), "Refusing a nonempty unmarked test directory")
            marker.write_text(str(self.lab.run) + "\n")
            (self.directory / "digibyte.conf").write_text("# This file belongs to the isolated Thaw Day test.\n")
        check(marker.read_text().strip() == str(self.lab.run), "Wrong test directory owner")
        command = [str(self.lab.client / "src/qt/digibyte-qt"), "-testnet", "-easypow",
                   f"-datadir={self.directory}", f"-conf={self.directory / 'digibyte.conf'}",
                   "-settings=0", "-server=1", "-listen=1", "-listenonion=0", "-dnsseed=0",
                   "-fixedseeds=0", "-discover=0", "-upnp=0", "-natpmp=0", "-onion=0",
                   "-proxy=0", "-bind=127.0.0.1", "-rpcbind=127.0.0.1", "-rpcallowip=127.0.0.1",
                   f"-port={self.record['p2p_port']}", f"-rpcport={self.record['rpc_port']}",
                   "-digidollar=1", "-txindex=1", f"-digidollarstatsindex={self.index % 2}",
                   "-fallbackfee=0.0001", "-dandelion=0", "-dbcache=32", "-par=1",
                   "-maxconnections=64", "-debug=digidollar", "-assumevalid=0", "-min=0", "-splash=0"]
        command.extend(self.record.get("extra_args", []))
        command.append("-connect=0" if self.index == 0 else
                       f"-connect=127.0.0.1:{self.lab.nodes[0].record['p2p_port']}")
        if (self.directory / "testnet26" / "wallets" / self.wallet).exists():
            command.append("-wallet=" + self.wallet)
        if reindex:
            command.append("-reindex=1")
        environment = os.environ.copy()
        environment.update(THAWDAY_LAB="1", THAWDAY_FEED_URL=f"http://127.0.0.1:{self.lab.state['feed_port']}")
        environment["XDG_CONFIG_HOME"] = str(self.directory / "qt-preferences")
        output = (self.lab.run / f"{self.name}-console.log").open("ab")
        self.process = subprocess.Popen(command, env=environment, stdout=output, stderr=subprocess.STDOUT,
                                        start_new_session=True)
        output.close()
        self.record.update(pid=self.process.pid, start_time=process_identity(self.process.pid), command=command)
        self.lab.save_state()

        def ready():
            check(self.process.poll() is None, f"{self.name} stopped. Read its console log.")
            try:
                self.rpc("getblockchaininfo")
                return True
            except (OSError, urllib.error.URLError):
                return False
            except RPCError as error:
                if error.code == -28:
                    return False
                raise
        wait_for(ready, self.name + " startup", 600)
        if self.wallet not in self.rpc("listwallets"):
            self.rpc("createwallet", self.wallet)
        self.record["running"] = True
        if self.index == 0:
            # A restarted miner starts its oracle signing session over. A price
            # already in the chain does not prove that the miner can sign a new
            # one, and a block that carries a mint or redemption needs a fresh
            # signed price. Only a signed price in a block mined after this
            # start counts as ready.
            self.lab.miner_started_at = self.rpc("getblockcount")
        self.lab.save_state()

    def stop(self):
        if not self.record.get("pid"):
            return
        identity = process_identity(self.record["pid"])
        if identity is None:
            self.record["running"] = False
            return
        check(identity == self.record["start_time"], "Test process identity changed")
        try:
            self.rpc("stop")
        except (OSError, urllib.error.URLError, RPCError):
            # A startup error can happen before RPC opens. Signal only the
            # exact process this rehearsal started, never a process-name match.
            os.kill(self.record["pid"], signal.SIGTERM)
        if self.process:
            self.process.wait(timeout=180)
        else:
            wait_for(lambda: process_identity(self.record["pid"]) is None, self.name + " shutdown")
        self.record["running"] = False
        self.lab.save_state()

    def start_oracles(self):
        for slot in SLOTS[self.index] if self.index < len(SLOTS) else []:
            key = hashlib.sha256(f"digibyte_testnet_oracle_{slot}".encode()).hexdigest()
            result = self.rpc("startoracle", slot, key)
            check(result["success"] is True, f"Test oracle {slot} failed to start")

    def balance(self):
        return self.rpc("getdigidollarbalance", wallet=True)["confirmed"]


class Lab:
    def __init__(self, run, client=None):
        self.run = run.resolve()
        self.state = json.loads((run / "run.json").read_text()) if (run / "run.json").exists() else {}
        self.client = Path(client or self.state["client"]).resolve()
        self.nodes = [Node(self, index, record) for index, record in enumerate(self.state.get("nodes", []))]
        self.expected_supply = 0
        self.expected_principal = 0
        self.open_positions = {}
        self.dd_transactions = {}
        self.vaults = []
        self.phase = "setup"
        self.miner_started_at = -1

    def save_state(self):
        save(self.run / "run.json", self.state)

    def note(self, label, evidence):
        print(label, flush=True)
        with (self.run / "results.jsonl").open("a") as output:
            output.write(json.dumps({"time": time.time(), "check": label, "evidence": evidence}, default=str) + "\n")

    def setup(self):
        check(os.environ.get("DISPLAY"), "A desktop DISPLAY is needed for the eight test Qt windows")
        binaries = [self.client / name for name in ("src/qt/digibyte-qt", "src/digibyte-cli", "src/digibyted")]
        check(all(path.is_file() for path in binaries), "Build the separate Thaw Day client first")
        manifest = json.loads((self.client / "THAWDAY_BUILD.json").read_text())
        patch = (ROOT / "contrib/thawday/client.patch").read_bytes()
        check(b"THAWDAY_LAB" in patch and b"THAWDAY_FEED_URL" in patch,
              "This is not the guarded Thaw Day test client")
        check(hashlib.sha256(patch).hexdigest() == manifest["patch_sha256"], "The test patch changed after the build")
        actual_base = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=self.client, text=True).strip()
        check(actual_base == manifest["base_commit"], "The test client base changed after the build")
        for entry in manifest["changed_files"] + manifest["binaries"]:
            check(digest(self.client / entry["path"]) == entry["sha256"], "A test source or binary changed after the recorded build")
        changed = set(subprocess.check_output(["git", "diff", "--name-only", "HEAD"], cwd=self.client, text=True).splitlines())
        check(changed <= {entry["path"] for entry in manifest["changed_files"]}, "Unexpected changes in the test client")
        self.state = {"client": str(self.client), "run_dir": str(self.run), "height": H,
                      "base_commit": subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=self.client, text=True).strip(),
                      "binary_sha256": {str(path): digest(path) for path in binaries},
                      "original_script_sha256": digest(ROOT / "test_multi_oracle_testnet.sh"),
                      "harness_sha256": digest(Path(__file__)),
                      "feed_port": free_port(), "nodes": [], "status": "RUNNING"}
        (self.run / "rehearsal.py").write_bytes(Path(__file__).read_bytes())
        (self.run / "test-client.patch").write_bytes(patch)
        (self.run / "price-micro-usd.txt").write_text(str(BASE_PRICE))
        used = {self.state["feed_port"]}
        for name in NAMES:
            ports = []
            while len(ports) < 2:
                port = free_port()
                if port not in used:
                    used.add(port)
                    ports.append(port)
            self.state["nodes"].append({"name": name, "datadir": str(self.run / name),
                                        "p2p_port": ports[0], "rpc_port": ports[1]})
        self.save_state()
        guard_directory = self.run / "guard-checks"
        guard_directory.mkdir()
        guard_config = guard_directory / "digibyte.conf"
        guard_config.write_text("# Empty test configuration.\n")
        checks = ((["-chain=main", "-easypow"], "1", "http://127.0.0.1:1", "only -testnet -easypow"),
                  (["-testnet", "-easypow"], "0", "http://127.0.0.1:1", "set THAWDAY_LAB=1"),
                  (["-testnet", "-easypow"], "1", "http://example.invalid:80", "must be http://127.0.0.1"))
        for flags, enabled, feed_url, expected in checks:
            environment = os.environ.copy()
            environment.update(THAWDAY_LAB=enabled, THAWDAY_FEED_URL=feed_url)
            result = subprocess.run([str(self.client / "src/digibyted"), f"-datadir={guard_directory}",
                                     f"-conf={guard_config}", "-settings=0", "-connect=0", "-listen=0"] + flags,
                                    env=environment, capture_output=True, text=True, timeout=20)
            check(result.returncode != 0 and expected in result.stderr, "A test-client startup guard did not refuse unsafe settings")
        self.note("The test client refuses public mode and missing or public feed settings", {})
        feed_log = (self.run / "feed-console.log").open("ab")
        feed = subprocess.Popen([sys.executable, str(Path(__file__).resolve()), "feed", "--run-dir", str(self.run)],
                                stdout=feed_log, stderr=subprocess.STDOUT, start_new_session=True)
        feed_log.close()
        self.state["feed_pid"] = feed.pid
        self.state["feed_start_time"] = process_identity(feed.pid)
        self.save_state()
        self.nodes = [Node(self, index, record) for index, record in enumerate(self.state["nodes"])]
        for node in self.nodes:
            node.start()
        from wallet_taproot import KEYS
        from test_framework.descriptors import descsum_create
        imported = self.nodes[0].rpc("importdescriptors", [{
            "desc": descsum_create(f"tr({KEYS[0]['xprv']}/*)"), "timestamp": "now",
            "active": True, "internal": False, "range": [0, 63], "next_index": 0,
        }], wallet=True)
        check(imported[0]["success"], "The public test owner-key fixture could not be imported")
        self.mining_address = self.nodes[0].rpc("getnewaddress", "Thaw Day test mining", "bech32", wallet=True)
        self.state["mining_address"] = self.mining_address
        self.save_state()
        self.note("Eight separate test wallets are open", {"run_dir": str(self.run)})

    def sync(self):
        tip = self.nodes[0].rpc("getbestblockhash")
        wait_for(lambda: all(n.rpc("getbestblockhash") == tip for n in self.nodes if n.record.get("running")),
                 "test nodes to agree on the exact block hash", 300)
        for node in self.nodes:
            if node.record.get("running"):
                node.rpc("syncwithvalidationinterfacequeue")

    def reconnect_hub(self):
        # After a node reindexes, the peer connections it accepted while still in
        # initial block download stop relaying new blocks. The hub (node 0)
        # accepts the spokes' connections, so a hub reindex leaves every spoke
        # connected but not receiving new blocks. Re-establish each running
        # spoke's link to the hub so block relay resumes cleanly.
        hub = self.nodes[0]
        addr = f"127.0.0.1:{hub.record['p2p_port']}"
        for node in self.nodes[1:]:
            if not node.record.get("running"):
                continue
            for method, params in (("disconnectnode", [addr]), ("addnode", [addr, "onetry"])):
                try:
                    node.rpc(method, *params)
                except RPCError:
                    pass
        wait_for(lambda: all(n.rpc("getconnectioncount") >= 1 for n in self.nodes[1:]
                             if n.record.get("running")),
                 "spokes to reconnect to the hub", 60)

    def mine(self, count=1, ceiling=None):
        height = self.nodes[0].rpc("getblockcount")
        check(ceiling is None or height + count <= ceiling, "Test helper would cross Thaw Day too early")
        result = self.nodes[0].rpc("generatetoaddress", count, self.mining_address, 2000000000, "sha256d", wallet=True)
        check(len(result) == count, "Mining returned fewer blocks than requested")
        self.sync()
        return result

    def mine_to(self, target):
        while self.nodes[0].rpc("getblockcount") < target:
            current = self.nodes[0].rpc("getblockcount")
            self.mine(min(40, target - current), ceiling=target)
            if current % 400 == 0:
                self.note("Advancing the local test chain", {"height": self.nodes[0].rpc("getblockcount")})

    def price(self, price):
        temporary = self.run / "price-micro-usd.new"
        temporary.write_text(str(price))
        temporary.replace(self.run / "price-micro-usd.txt")

    def await_quote(self, price, ceiling=None):
        # A restarted miner signs its next price in the next 40-block epoch,
        # so the wait must cover a whole epoch plus the signing round.
        deadline = time.monotonic() + 420
        while time.monotonic() < deadline:
            views = [node.rpc("getprotectionstatus")["volatility"] for node in self.nodes[:3]]
            height = self.nodes[0].rpc("getblockcount")
            signed = self.signed_bundle(height)
            if (signed and signed["price_micro_usd"] == price and signed["epoch"] == (height + 1) // 40 and
                    signed["height"] > self.miner_started_at and
                    all(view["quote_available"] and view["candidate_price_micro_usd"] == price for view in views)):
                self.note("Candidate price is ready", {"price_micro_usd": price, "signed_height": signed["height"],
                          "signers": signed["signers"], "miner_started_at": self.miner_started_at, "views": views})
                return
            if ceiling is None or self.nodes[0].rpc("getblockcount") < ceiling:
                self.mine(1, ceiling=ceiling)
            time.sleep(3)
        raise Failure(f"No signed candidate quote at {price} micro-USD before the deadline")

    def signed_bundle(self, height, expected=None):
        sys.path.insert(0, str(ROOT / "test/functional"))
        from test_framework.script import CScript
        node = self.nodes[0]
        block_hash = node.rpc("getblockhash", height)
        block = node.rpc("getblock", block_hash, 2)
        scripts = [bytes.fromhex(out["scriptPubKey"]["hex"]) for out in block["tx"][0]["vout"]]
        matches = [script for script in scripts if script.startswith(b"\x6a\xbf")]
        if not matches:
            return None
        check(len(matches) == 1, "More than one oracle bundle in a coinbase")
        fields = list(CScript(matches[0]))
        check(len(fields) == 4 and fields[2] == b"\x03", "Unexpected oracle bundle format")
        payload = fields[3]
        width = payload[0]
        check(len(payload) == 1 + width + 4 + 8 + 8 + 64, "Malformed oracle bundle length")
        bitmap = int.from_bytes(payload[1:1 + width], "little")
        check(bitmap.bit_count() >= 7 and bitmap >> 24 == 0, "Unexpected oracle signer set")
        offset = 1 + width + 4
        price = int.from_bytes(payload[offset:offset + 8], "little")
        check(expected is None or expected == price, "Signed block price differs from the expected price")
        # All nodes must accept this block through their normal validation path.
        check(all(n.rpc("getblockhash", height) == block_hash for n in self.nodes), "Nodes disagree on the signed block")
        return {"height": height, "hash": block_hash, "price_micro_usd": price,
                "epoch": int.from_bytes(payload[1 + width:1 + width + 4], "little"),
                "signers": [slot for slot in range(24) if bitmap & (1 << slot)]}

    def confirm(self, node, result, ceiling=None, attempts=12, record=True):
        # record=False for an ordinary DGB tx (e.g. a control payment) that must
        # not enter dd_transactions, which the accounting pass decodes as DD.
        txid = result["txid"] if isinstance(result, dict) else result
        for _ in range(attempts):
            tx = node.rpc("gettransaction", txid, wallet=True)
            if tx["confirmations"] > 0:
                self.sync()
                if record:
                    self.dd_transactions[txid] = tx["hex"]
                block = node.rpc("getblock", tx["blockhash"])
                check(txid in block["tx"], "Wallet confirmation does not match the block bytes")
                check(self.phase != "before" or block["height"] < H, "A before-Thaw transaction confirmed too late")
                check(self.phase != "after" or block["height"] >= H, "An after-Thaw transaction confirmed too early")
                self.note("Confirmed " + self.phase + " transaction", {"txid": txid,
                          "block_hash": block["hash"], "height": block["height"]})
                return txid
            self.mine(1, ceiling=ceiling)
            time.sleep(1)
        raise Failure("Transaction did not confirm: " + txid)

    def expect_error(self, node, method, params, words):
        before = sorted(node.rpc("getrawmempool"))
        try:
            node.rpc(method, *params, wallet=True)
        except RPCError as error:
            check(any(word.lower() in str(error).lower() for word in words),
                  f"Wrong rejection for {method}: {error}")
            check(before == sorted(node.rpc("getrawmempool")), "Rejected call changed the mempool")
            return {"code": error.code, "reason": str(error)}
        raise Failure("Expected a rejection from " + method)

    def account(self, phase, active):
        self.sync()
        from test_framework.messages import tx_from_hex
        from test_framework.script import CScript, OP_RETURN
        from digidollar_thawday_extra_burn import metadata, script_number
        tokens = 0
        for txid, raw in self.dd_transactions.items():
            transaction = tx_from_hex(raw)
            # A full redemption with no DD change carries no DD OP_RETURN and no
            # token output. It is a valid redemption (consensus accepts it) that
            # contributes nothing to the circulating token count, so skip it
            # rather than demand the metadata every other DD tx has.
            dd_marked = [out for out in transaction.vout
                         if out.scriptPubKey[:1] == bytes([OP_RETURN])
                         and len(list(CScript(out.scriptPubKey))) >= 2
                         and list(CScript(out.scriptPubKey))[1] == b"DD"]
            token_indices = [index for index, out in enumerate(transaction.vout)
                             if out.nValue == 0 and len(out.scriptPubKey) == 34
                             and out.scriptPubKey[:2] == b"\x51\x20"]
            if not dd_marked:
                check(not token_indices, "A DD tx without metadata unexpectedly has token outputs")
                continue
            _, fields = metadata(transaction)
            kind = script_number(fields[2])
            amounts = [script_number(value) for value in (fields[3:] if kind == 2 else fields[3:4])]
            indices = [index for index, out in enumerate(transaction.vout)
                       if out.nValue == 0 and len(out.scriptPubKey) == 34 and out.scriptPubKey[:2] == b"\x51\x20"]
            if kind == 3 and amounts == [0]:
                amounts = []
            check(len(indices) == len(amounts), "Independent token count cannot decode this transaction")
            tokens += sum(amount for index, amount in zip(indices, amounts)
                          if self.nodes[0].rpc("gettxout", txid, index, False) is not None)
        principal, collateral, vault_count = 0, 0, 0
        for txid, index, original in self.vaults:
            coin = self.nodes[0].rpc("gettxout", txid, index, False)
            if coin is not None:
                principal += original
                collateral += int(coin["value"] * 100000000)
                vault_count += 1
        check(tokens == self.expected_supply, "Independent token coins disagree with the test ledger")
        check(principal == self.expected_principal and vault_count == len(self.open_positions),
              "Independent vault coins disagree with the test ledger")
        records = []
        for node in self.nodes:
            stats = node.rpc("getdigidollarstats")
            # Below Thaw Day, exact circulating supply is only guaranteed on a node
            # with a synchronized stats index. Without it, getdigidollarstats uses
            # the documented legacy fallback, a vault-based figure that after a
            # restart does not subtract extra burns (see CLAUDE.md). At and after
            # Thaw Day the chainstate owns the supply and it is exact on every node.
            # The independent on-chain token count above already checks true
            # circulating against the chain, so this only asserts the node's field
            # where it is contractually exact.
            if active or node.index % 2 == 1:
                check(stats["total_dd_supply"] == self.expected_supply, f"Wrong circulating DD on {node.name}")
            check(stats["active_positions"] == len(self.open_positions), f"Wrong vault count on {node.name}")
            check(int(stats["total_collateral_locked"] * 100000000) == collateral, "Wrong collateral total")
            if active:
                record = stats["canonical_health"]
                check(record["ready"] and record["history_checked"], "Saved health state is not ready")
                check(record["format_version"] == 1 and record["rules_version"] == 1 and
                      record["activation_height"] == H and record["digidollar_height"] == 600 and
                      record["genesis_hash"] == node.rpc("getblockhash", 0), "Health record has the wrong rules or network")
                check(record["block_hash"] == node.rpc("getbestblockhash"), "Health record belongs to a different block")
                check(record["open_vault_principal"] == self.expected_principal, "Wrong principal in open vaults")
                check(record["collateral"] == collateral and record["active_vaults"] == vault_count, "Wrong saved vault totals")
                candidate = node.rpc("getprotectionstatus")["next_block_health"]
                check(candidate["ready"], "Candidate health is not ready at the accounting checkpoint")
                if candidate["ready"]:
                    check(candidate["canonical_health"] == record, "Consensus view differs from the statistics view")
                    quote = candidate["oracle_price_micro_usd"]
                    health = min(30000, collateral * (quote // 10) * 100 // (100000000 * 1000 * principal)) if principal else 30000
                    check(candidate["health_percentage"] == health, "Candidate health uses the wrong amounts or quote")
                check(stats["selected_health_denominator"] == "open_vault_principal", "Wrong post-Thaw health definition")
                records.append(record)
            else:
                check(stats["selected_health_denominator"] == "legacy_supply", "New health definition used too early")
        if records:
            check(all(record == records[0] for record in records), "Nodes disagree on the durable health record")
        self.note(phase + ": accounting matches", {"circulating_cents": self.expected_supply,
                  "open_principal_cents": self.expected_principal, "vaults": len(self.open_positions),
                  "canonical_record": records[0] if records else None})

    def recover_wallet(self, node, phase):
        amounts = lambda data: {key: data[key] for key in ("confirmed", "unconfirmed", "total")}
        before = amounts(node.rpc("getdigidollarbalance", wallet=True))
        positions = node.rpc("listdigidollarpositions", False, wallet=True)
        backup = self.run / f"{phase}-{node.name}-wallet.dat"
        node.rpc("backupwallet", str(backup), wallet=True)
        check(backup.is_file() and backup.stat().st_size > 0, "Wallet backup was not written")
        node.stop()
        node.start()
        node.start_oracles()
        self.sync()
        check(amounts(node.rpc("getdigidollarbalance", wallet=True)) == before, "Wallet balance changed after restart")
        restored_name = node.wallet + "-restored-" + phase
        node.rpc("restorewallet", restored_name, str(backup))
        original = node.wallet
        node.wallet = restored_name
        try:
            node.rpc("rescanblockchain", wallet=True)
            check(amounts(node.rpc("getdigidollarbalance", wallet=True)) == before, "Backup restore changed DD balance")
            after = node.rpc("listdigidollarpositions", False, wallet=True)
            keys = ("position_id", "dd_minted", "lock_tier", "is_active", "status")
            simplify = lambda items: sorted([{key: item.get(key) for key in keys} for item in items], key=lambda item: item["position_id"])
            check(simplify(after) == simplify(positions), "Backup restore changed vault ownership or status")
        finally:
            node.wallet = original
        node.rpc("unloadwallet", restored_name)
        self.note(phase + ": wallet restart and backup restore match", {"wallet": node.name, "backup_sha256": digest(backup)})

    def core_suite(self, phase, price, active):
        self.phase = phase
        self.note("Starting the " + phase + " DigiDollar tests", {"price_micro_usd": price})
        self.await_quote(price, None if active else H - 1000)
        bob, alice, charlie = self.nodes[:3]
        positions = []
        for tier in list(range(10)) + [0, 0]:
            result = bob.rpc("mintdigidollar", 10000, tier, wallet=True)
            txid = self.confirm(bob, result, None if active else H - 1000)
            position = next(item for item in bob.rpc("listdigidollarpositions", wallet=True) if item["position_id"] == txid)
            check(position["dd_minted"] == 10000 and position["lock_tier"] == tier, "Minted vault has wrong terms")
            self.open_positions[txid] = position
            from test_framework.messages import tx_from_hex
            raw = tx_from_hex(self.dd_transactions[txid])
            collateral_indices = [index for index, out in enumerate(raw.vout)
                                  if out.nValue > 0 and len(out.scriptPubKey) == 34 and out.scriptPubKey[:2] == b"\x51\x20"]
            check(len(collateral_indices) == 1, "Mint has no unique collateral output")
            self.vaults.append((txid, collateral_indices[0], 10000))
            positions.append(txid)
            self.expected_supply += 10000
            self.expected_principal += 10000
        self.note(phase + ": all ten mint tiers confirmed", {"positions": positions})
        early = self.expect_error(bob, "redeemdigidollar", [positions[0], 10000], ["Position locked until block"])
        self.note(phase + ": early redemption rejected", early)
        balances = [node.balance() for node in (bob, alice, charlie)]
        for sender, receiver, amount in ((bob, alice, 2500), (alice, charlie, 1000), (charlie, bob, 500)):
            address = receiver.rpc("getdigidollaraddress", wallet=True)
            self.confirm(sender, sender.rpc("senddigidollar", address, amount, wallet=True), None if active else H - 1000)
        expected = [balances[0] - 2000, balances[1] + 1500, balances[2] + 500]
        check([node.balance() for node in (bob, alice, charlie)] == expected, "Send/receive totals do not match")
        self.note(phase + ": send and receive balances match", {"before": balances, "after": expected})
        unlock = max(self.open_positions[txid]["unlock_height"] for txid in (positions[0], positions[10], positions[11])) + 1
        check(active or unlock < H - 1000, "The pre-Thaw vaults would mature too close to H")
        self.mine_to(unlock)
        self.await_quote(price, None if active else H - 1000)
        partial = self.expect_error(bob, "redeemdigidollar", [positions[0], 5000], ["partial", "full", "entire", "amount"])
        self.note(phase + ": partial vault redemption rejected", partial)
        before = bob.balance()
        self.confirm(bob, bob.rpc("redeemdigidollar", positions[0], 10000, wallet=True), None if active else H - 1000)
        check(bob.balance() == before - 10000, "Normal redemption burned the wrong amount")
        self.expected_supply -= 10000
        self.expected_principal -= 10000
        del self.open_positions[positions[0]]
        again = self.expect_error(bob, "redeemdigidollar", [positions[0], 10000], ["redeemed", "active", "spent", "found"])
        self.note(phase + ": full redemption confirmed; second redemption rejected", again)
        self.state[phase + "_spare_position"] = positions[11]
        self.state[phase + "_extra_burn_position"] = positions[10]
        self.save_state()
        from extra_burn import apply_extra_burn_case
        apply_extra_burn_case(self, phase, price, active)
        self.await_quote(price, None if active else H - 1000)
        self.account(phase, active)
        self.recover_wallet(bob, phase)
        self.recover_wallet(alice, phase)
        self.await_quote(price, None if active else H - 1000)
        self.account(phase + " after recovery", active)

    def mint_record(self, node, amount, tier):
        from test_framework.messages import tx_from_hex
        result = node.rpc("mintdigidollar", amount, tier, wallet=True)
        txid = self.confirm(node, result)
        position = next(item for item in node.rpc("listdigidollarpositions", wallet=True)
                        if item["position_id"] == txid)
        raw = tx_from_hex(self.dd_transactions[txid])
        indices = [index for index, out in enumerate(raw.vout)
                   if out.nValue > 0 and len(out.scriptPubKey) == 34 and out.scriptPubKey[:2] == b"\x51\x20"]
        check(len(indices) == 1, "Mint has no unique collateral output")
        self.vaults.append((txid, indices[0], amount))
        self.open_positions[txid] = position
        self.expected_supply += amount
        self.expected_principal += amount
        return txid

    def mint_only_pause(self):
        # This runs entirely after Thaw Day, so there is no boundary to keep
        # blocks below. Each price edge (and restoring 45000 at the end) needs a
        # full oracle epoch to re-sign, more than a tight block cap allows, so
        # let await_quote mine as many blocks as the signing round needs.
        ceiling = None
        bob, alice, charlie = self.nodes[:3]
        for price, restricted, spare in ((54000, True, "after"), (53999, False, None),
                                          (36000, True, "before"), (36001, False, None)):
            self.price(price)
            self.await_quote(price, ceiling)
            status = bob.rpc("getprotectionstatus")["volatility"]
            check(status["reference_price_micro_usd"] == NEW_PRICE, "The fixed reference moved during the edge tests")
            check(status["minting_restricted"] is restricted, "Wrong result at a 20 percent boundary")
            check(status["all_operations_restricted"] is False, "A mint pause blocked all operations")
            if restricted:
                rejection = self.expect_error(bob, "mintdigidollar", [10000, 0], ["Minting volatility pause"])
                balance = [alice.balance(), charlie.balance()]
                self.confirm(alice, alice.rpc("senddigidollar", charlie.rpc("getdigidollaraddress", wallet=True), 100, wallet=True))
                self.confirm(charlie, charlie.rpc("senddigidollar", alice.rpc("getdigidollaraddress", wallet=True), 100, wallet=True))
                check([alice.balance(), charlie.balance()] == balance, "A transfer during the mint pause changed value")
                position = self.state[spare + "_spare_position"]
                result = bob.rpc("redeemdigidollar", position, 10000, wallet=True)
                check(result["err_active"] is False and result["required_dd_burn"] == 10000,
                      "Mint-pause redemption did not take the expected normal route")
                self.confirm(bob, result)
                self.expected_supply -= 10000
                self.expected_principal -= 10000
                del self.open_positions[position]
                self.note("Mint pause rejects mint but allows send and eligible redemption", {
                    "price_micro_usd": price, "status": status, "rejection": rejection,
                    "redemption": result})
            else:
                txid = self.mint_record(bob, 10000, 0)
                self.note("A mint just inside the price boundary confirmed", {"price_micro_usd": price, "txid": txid})
            self.await_quote(price, ceiling)
            self.account("Price boundary " + str(price), True)
        self.price(NEW_PRICE)
        self.await_quote(NEW_PRICE, ceiling)

    def boundary(self):
        self.phase = "boundary"
        self.price(NEW_PRICE)
        self.await_quote(NEW_PRICE, H - 300)
        frozen = self.nodes[0].rpc("getprotectionstatus")["volatility"]
        check(frozen["rejection_reason"] == "legacy_volatility_freeze", "The legacy freeze was not reproduced")
        rejected = self.expect_error(self.nodes[0], "mintdigidollar", [10000, 0], ["volatility", "frozen", "freeze"])
        self.note("A fresh signed price still hits the old mint freeze", {"status": frozen, "mint_rejection": rejected})
        self.mine_to(H - 280)
        self.await_quote(NEW_PRICE, H - 260)
        self.mine_to(H - 260)
        samples = []
        for _ in range(20):
            block = self.mine(1, ceiling=H - 240)[0]
            sample = self.signed_bundle(self.nodes[0].rpc("getblockheader", block)["height"])
            if sample and sample["price_micro_usd"] == NEW_PRICE:
                samples.append(sample)
            time.sleep(1)
        check(len(samples) >= 15, "Need at least fifteen signed new-price ancestor samples")
        save(self.run / "ancestor-samples.json", samples)
        self.mine_to(H - 40)
        self.await_quote(NEW_PRICE, H - 10)
        self.mine_to(H - 2)
        self.await_quote(NEW_PRICE, H - 2)
        before = self.nodes[0].rpc("getdigidollardeploymentinfo")["thaw_day"]
        check(not before["active_at_tip"] and not before["active_next_block"], "Thaw Day started before H")
        self.expect_error(self.nodes[0], "mintdigidollar", [10000, 0], ["volatility", "frozen", "freeze"])
        self.note("Candidate H-1 still uses the old rules", before)
        self.mine(1, ceiling=H - 1)
        boundary = self.nodes[0].rpc("getdigidollardeploymentinfo")["thaw_day"]
        check(not boundary["active_at_tip"] and boundary["active_next_block"], "Candidate H did not select Thaw Day")
        self.note("Tip H-1 selects the new rules for the next block", boundary)
        # H is also an oracle epoch boundary. An empty block can advance the
        # chain while the next signing session gathers a valid quote.
        self.mine(1, ceiling=H)
        after = self.nodes[0].rpc("getdigidollardeploymentinfo")["thaw_day"]
        check(after["active_at_tip"] and after["active_next_block"], "Block H did not activate Thaw Day")
        self.note("Block H uses the new rules", after)
        self.await_quote(NEW_PRICE)
        volatility = self.nodes[0].rpc("getprotectionstatus")["volatility"]
        check(volatility["reference_price_micro_usd"] == NEW_PRICE and volatility["sample_count"] == 15,
              "The new rule did not use the signed ancestor samples")
        check(not volatility["minting_restricted"] and not volatility["all_operations_restricted"], "The old freeze survived Thaw Day")
        self.account("Thaw Day boundary", True)
        self.note("The old freeze clears under the new ancestor rule", volatility)

    def reindex_node(self, node, phase):
        """Reindex one node with its wallet loaded and require the same tip."""
        tip = self.nodes[0].rpc("getbestblockhash")
        height = self.nodes[0].rpc("getblockcount")
        balance = node.balance()
        node.stop()
        node.start(reindex=True)
        wait_for(lambda: node.rpc("getblockcount") == height and node.rpc("getbestblockhash") == tip,
                 node.name + " to finish its full reindex", 600)
        node.start_oracles()
        self.reconnect_hub()
        self.sync()
        check(node.balance() == balance, node.name + " changed its DD balance across the reindex")
        self.note(phase + ": " + node.name + " reindexed to the same block with its wallet loaded",
                  {"height": height, "hash": tip, "dd_balance_cents": balance})

    def reindex_and_fresh_sync(self):
        # Bob minted and sent DigiDollar, so its wallet holds reused owner
        # addresses. A reindex of Bob is the check that would have caught the
        # rc1 defect. Heidi never held DigiDollar and reindexes as before.
        self.reindex_node(self.nodes[0], "End of run")
        self.await_quote(NEW_PRICE)
        node = self.nodes[7]
        tip = self.nodes[0].rpc("getbestblockhash")
        height = self.nodes[0].rpc("getblockcount")
        node.stop()
        node.start(reindex=True)
        wait_for(lambda: node.rpc("getblockcount") == height and node.rpc("getbestblockhash") == tip,
                 "the small test chain to finish its full reindex", 600)
        node.start_oracles()
        self.reconnect_hub()
        self.sync()
        self.account("After full test-chain reindex", True)
        fresh = {"name": "fresh", "datadir": str(self.run / "fresh"), "p2p_port": free_port(), "rpc_port": free_port()}
        self.state["nodes"].append(fresh)
        observer = Node(self, len(self.nodes), fresh)
        self.nodes.append(observer)
        observer.start()
        self.sync()
        self.account("After empty-directory fresh sync", True)
        self.note("Fresh sync and full test-chain reindex match", {"height": height, "hash": tip})

    def boundary_reorg(self):
        """Replace the boundary blocks without changing any validation rule."""
        original_tip = self.nodes[0].rpc("getbestblockhash")
        original_height = self.nodes[0].rpc("getblockcount")
        first_removed = self.nodes[0].rpc("getblockhash", H - 1)
        parent = self.nodes[0].rpc("getblockhash", H - 2)
        before = self.nodes[0].rpc("gettxoutsetinfo", "muhash")
        follower = self.nodes[7]
        follower.rpc("setnetworkactive", False)
        follower.rpc("invalidateblock", first_removed)
        check(follower.rpc("getbestblockhash") == parent, "Follower did not rewind below the boundary")
        status = follower.rpc("getdigidollardeploymentinfo")["thaw_day"]
        check(not status["active_at_tip"] and not status["active_next_block"], "Follower kept activated rules below H")
        follower.stop()
        follower.record["extra_args"] = ["-networkactive=0"]
        follower.start()
        check(follower.rpc("getbestblockhash") == parent, "Restart changed the intentionally rewound test branch")
        follower.rpc("reconsiderblock", first_removed)
        check(follower.rpc("getbestblockhash") == original_tip, "Follower did not restore the original chain")
        after = follower.rpc("gettxoutsetinfo", "muhash")
        check(before["muhash"] == after["muhash"], "Rewind and replay changed the UTXO set")
        follower.record["extra_args"] = []
        follower.rpc("setnetworkactive", True)
        follower.start_oracles()
        self.account("After boundary rewind, restart, and replay", True)
        # Disable peer traffic while building the controlled competing branch.
        # Each follower still checks submitted raw blocks through normal validation.
        for node in self.nodes:
            node.rpc("setnetworkactive", False)
            node.rpc("invalidateblock", first_removed)
            check(node.rpc("getbestblockhash") == parent, "A node did not reach the fork parent")
        address = self.nodes[0].rpc("getnewaddress", "Thaw Day replacement branch", "bech32", wallet=True)
        replacement = []
        for _ in range(original_height - (H - 2) + 2):
            block_hash = self.nodes[0].rpc("generatetoaddress", 1, address, 2000000000, "sha256d", wallet=True)[0]
            raw = self.nodes[0].rpc("getblock", block_hash, 0)
            for node in self.nodes[1:]:
                result = node.rpc("submitblock", raw)
                check(result is None, f"Replacement block was not accepted on {node.name}: {result}")
                check(node.rpc("getbestblockhash") == block_hash, "Replacement block did not become the tip")
            replacement.append(block_hash)
        for node in self.nodes:
            node.rpc("reconsiderblock", first_removed)
            check(node.rpc("getbestblockhash") == replacement[-1], "Node chose the shorter original branch")
            node.rpc("setnetworkactive", True)
        self.sync()
        self.await_quote(NEW_PRICE)
        self.account("After a replacement branch crosses Thaw Day", True)
        self.note("Boundary rollback and competing-branch tests passed", {
            "old_tip": original_tip, "fork_parent": parent, "replacement_blocks": replacement,
            "restored_utxo_muhash": after["muhash"],
            "limit": "This short branch test does not replace the older price-sample ancestors."})

    def run_tests(self):
        self.setup()
        self.mine_to(3000)
        for node in self.nodes[1:]:
            address = node.rpc("getnewaddress", "Thaw Day test fees", "bech32", wallet=True)
            self.nodes[0].rpc("sendtoaddress", address, 100, wallet=True)
        self.mine(5)
        for node in self.nodes:
            node.start_oracles()
        wait_for(lambda: len([entry for entry in self.nodes[0].rpc("getoracles", True)
                              if entry.get("heartbeat_status") == "fresh"]) == 24,
                 "all twenty-four signed oracle heartbeats", 180)
        self.note("All twenty-four test oracles sent signed heartbeats", {})
        self.core_suite("before", BASE_PRICE, False)
        self.reindex_node(self.nodes[0], "before")
        self.boundary()
        self.boundary_reorg()
        self.core_suite("after", NEW_PRICE, True)
        self.mint_only_pause()
        self.reindex_and_fresh_sync()
        self.state["status"] = "PASS"
        self.state["final_height"] = self.nodes[0].rpc("getblockcount")
        self.state["final_hash"] = self.nodes[0].rpc("getbestblockhash")
        self.save_state()
        self.note("The implemented rehearsal checks passed", {"height": self.state["final_height"], "hash": self.state["final_hash"]})

    def stop(self):
        for node in reversed(self.nodes):
            node.stop()
        pid = self.state.get("feed_pid")
        if pid and process_identity(pid) == self.state.get("feed_start_time"):
            os.kill(pid, signal.SIGTERM)
        self.note("Only this rehearsal's test processes were stopped", {})


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=("run", "status", "stop", "feed"), nargs="?", default="run")
    parser.add_argument("--client", type=Path, default=ROOT / "builds/thawday-client")
    parser.add_argument("--run-dir", type=Path)
    parser.add_argument("--close-after", action="store_true", help="Close only the test windows when the run ends")
    args = parser.parse_args()
    os.umask(0o077)
    if args.command == "run":
        run = args.run_dir or ROOT / "builds/thawday-runs" / time.strftime("%Y%m%d-%H%M%S")
        check(not run.exists(), "Use a new run directory; previous evidence must be kept")
        run.mkdir(parents=True)
        lab = Lab(run, args.client)
        try:
            lab.run_tests()
        except Exception as error:
            lab.state["status"] = "FAIL"
            lab.state["error"] = str(error)
            lab.save_state()
            lab.note("The rehearsal stopped at a failed check", {"error": str(error)})
            raise
        finally:
            if args.close_after:
                lab.stop()
            else:
                print(f"Test windows stay open. Evidence: {run}", flush=True)
                print(f"Close only these test windows: ./thawDay.sh stop --run-dir {run}", flush=True)
    else:
        check(args.run_dir is not None, "Specify the rehearsal directory with --run-dir")
        if args.command == "feed":
            serve_feed(args.run_dir)
        elif args.command == "stop":
            Lab(args.run_dir).stop()
        else:
            print((args.run_dir / "run.json").read_text())


if __name__ == "__main__":
    main()
