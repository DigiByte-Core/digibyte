#!/usr/bin/env python3
# Copyright (c) 2026 The DigiByte Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""IsStandardTx must not write unconditional log lines for every transaction.

``IsStandardTx`` (src/policy/policy.cpp) runs for every transaction offered to
the mempool of every node that requires standard transactions (the default on
every network). It used to call ``LogPrintf`` there, which ignores ``-debug``
and cannot be switched off, so every node wrote at least one
``IsStandardTx: ...`` line per transaction and the log grew without bound.

This test submits an ordinary DGB payment and a DigiDollar mint (which takes
the DigiDollar-marker branch of the same function), confirms both were
accepted into the mempool (so ``IsStandardTx`` really ran), and asserts that
no ``IsStandardTx:`` line was written, neither during those submissions nor
anywhere else in the node's debug.log. The node runs with ``-debug`` (all
categories, the functional-test default) so category-gated logging is on;
the assertion is specifically about the uncategorised lines.
"""

from test_framework.test_framework import DigiByteTestFramework
from test_framework.util import assert_equal

FORBIDDEN_LOG_PREFIX = "IsStandardTx:"
ORACLE_PRICE_MICRO_USD = 500000  # $0.50 per DGB


class DigiDollarIsStandardTxNoLogTest(DigiByteTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True
        # -acceptnonstdtxn=0 is the default; it is spelled out so the test keeps
        # exercising IsStandardTx even if that default ever changes for regtest.
        self.extra_args = [
            ["-digidollar=1", "-txindex=1", "-mocktime=0", "-dandelion=0", "-acceptnonstdtxn=0"],
        ]

    def add_options(self, parser):
        self.add_wallet_options(parser)

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def run_test(self):
        node = self.nodes[0]

        self.log.info("Preparing an active DigiDollar chain")
        self.generate(node, 110)
        node.setmockoracleprice(ORACLE_PRICE_MICRO_USD)

        self.log.info("An ordinary DGB payment is accepted without an IsStandardTx: log line")
        with node.assert_debug_log(expected_msgs=[], unexpected_msgs=[FORBIDDEN_LOG_PREFIX]):
            txid = node.sendtoaddress(node.getnewaddress(), 1)
            assert txid in node.getrawmempool()

        self.log.info("A DigiDollar mint (marker transaction) is accepted without an IsStandardTx: log line")
        with node.assert_debug_log(expected_msgs=[], unexpected_msgs=[FORBIDDEN_LOG_PREFIX]):
            mint = node.mintdigidollar(10000, 0)
            assert mint["txid"] in node.getrawmempool()

        self.generate(node, 1)
        assert_equal(node.getrawmempool(), [])

        self.log.info("The whole debug.log contains no IsStandardTx: line at all")
        with open(node.debug_log_path, encoding="utf-8", errors="replace") as debug_log:
            offenders = [line.rstrip("\n") for line in debug_log if FORBIDDEN_LOG_PREFIX in line]
        assert_equal(offenders, [])


if __name__ == "__main__":
    DigiDollarIsStandardTxNoLogTest().main()
