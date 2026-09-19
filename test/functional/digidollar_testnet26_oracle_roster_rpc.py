#!/usr/bin/env python3
# Copyright (c) 2026 The DigiByte Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

"""RC44 testnet26 local mini-testnet oracle roster RPC regression.

The public RC44 testnet roster has 35 active MuSig2 oracle slots. The local
mini-testnet harness uses `-testnet -easypow`, which keeps the testnet26 chain
identity while swapping to 24 deterministic local oracle keys so the harness
can exercise quorum behavior without production private keys.

This checks all active slots, their quorum fields, and their local display
labels. Deterministic local keys must not inherit public operator names.
"""

import os

from test_framework.test_framework import DigiByteTestFramework
from test_framework.util import assert_equal, get_datadir_path, write_config


class DigiDollarTestnet26OracleRosterRPCTest(DigiByteTestFramework):
    def set_test_params(self):
        self.chain = "testnet26"
        self.num_nodes = 1
        self.setup_clean_chain = True
        self.rpc_timeout = 240
        self.extra_args = [[
            "-testnet",
            "-easypow",
            "-digidollar=1",
            "-txindex=1",
            "-dandelion=0",
        ]]

    def add_options(self, parser):
        self.add_wallet_options(parser)

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def setup_chain(self):
        self.log.info("Initializing testnet26 directory " + self.options.tmpdir)
        for i in range(self.num_nodes):
            datadir = get_datadir_path(self.options.tmpdir, i)
            os.makedirs(datadir, exist_ok=True)
            # The binary still parses this mode through -testnet; keep
            # TestNode.chain as testnet26 so cookie/log paths match base params.
            write_config(
                os.path.join(datadir, "digibyte.conf"),
                n=i,
                chain="testnet3",
                disable_autoconnect=self.disable_autoconnect,
            )
            os.makedirs(os.path.join(datadir, "stderr"), exist_ok=True)
            os.makedirs(os.path.join(datadir, "stdout"), exist_ok=True)

    def setup_network(self):
        self.add_nodes(self.num_nodes, self.extra_args)
        self.start_nodes()

    def run_test(self):
        node = self.nodes[0]

        self.log.info("Create a wallet and mine testnet26/easypow blocks to DigiDollar activation")
        node.createwallet("roster")
        wallet = node.get_wallet_rpc("roster")
        address = wallet.getnewaddress("mining", "bech32")
        while node.getblockcount() < 650:
            remaining = 650 - node.getblockcount()
            self.generatetoaddress(
                node,
                min(25, remaining),
                address,
                2_000_000_000,
                "sha256d",
                sync_fun=self.no_op,
            )
        assert_equal(node.getblockcount(), 650)

        deployment = node.getdigidollardeploymentinfo()
        assert_equal(deployment["enabled"], True)
        assert_equal(deployment["oracle_pubkey_count"], 24)
        assert_equal(deployment["oracle_consensus_required"], 7)
        assert_equal(deployment["oracle_total_slots"], 35)

        self.log.info("Verify getoracles labels all 24 active local test slots")
        active_oracles = node.getoracles(True, 20)
        assert_equal(len(active_oracles), 24)

        for oracle in active_oracles:
            oracle_id = oracle["oracle_id"]
            assert_equal(oracle["is_active"], True)
            assert_equal(oracle["in_consensus"], True)
            assert_equal(oracle["active_oracle_count"], 24)
            assert_equal(oracle["consensus_threshold"], 7)
            assert_equal(oracle["name"], f"Oracle {oracle_id}")


if __name__ == "__main__":
    DigiDollarTestnet26OracleRosterRPCTest().main()
