#!/usr/bin/env python3
# Copyright (c) 2026 The DigiByte Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Difficulty per mining algorithm on a real chain.

DigiByte has several mining algorithms and each one keeps its own difficulty. To
work out the difficulty for the next block of one algorithm, the node has to find
the most recent earlier block of that same algorithm. The rule is: walk back from
the block you are standing on, ignore blocks of other algorithms, and ignore
blocks stamped more than two block spacings after their parent, because those
were mined at the minimum difficulty on the networks that allow that.

This test mines a real regtest chain of 610 blocks with five algorithms, crossing
every height where the difficulty rules change and every height where an
algorithm is switched on or off, with blocks mined at the minimum difficulty
scattered through it, including runs of three next to those heights and one
algorithm whose every block was mined at the minimum difficulty. Then it checks:

  * every block was accepted, so the difficulty the miner used and the difficulty
    validation expected agreed on all 610 blocks;
  * the difficulty the node reports for each algorithm comes from the block the
    rule above picks, worked out here in Python from the block headers;
  * the same numbers come back after a restart and after a reindex, which is
    where a node rebuilds whatever it keeps in memory about earlier blocks of
    each algorithm.

What this test cannot show: regtest gives every block the easiest difficulty the
network allows (it sets fEasyPow), so every block carries the same difficulty and
the reported number is the same whichever block the lookup returns. The unit test
pow_algo_lookup_tests is what shows which block is returned.
"""

from test_framework.test_framework import DigiByteTestFramework
from test_framework.util import assert_equal

# regtest heights where something changes. A block may use one of the five
# MultiAlgo algorithms once its parent is at the MultiAlgo height, so the first
# block that can use them is one above it; the same shift applies to Odocrypt.
MULTIALGO_HEIGHT = 100   # the five algorithms are switched on
MULTISHIELD_HEIGHT = 200  # the difficulty rules change
DIGISPEED_HEIGHT = 400   # the difficulty rules change again
ODOCRYPT_HEIGHT = 600    # Odocrypt is switched on, Groestl is switched off

TIP_HEIGHT = 610

# A block counts as mined at the minimum difficulty when it is stamped more than
# twice the target spacing after its parent. The target spacing is 60 seconds on
# every DigiByte network, so the line is 120 seconds.
TARGET_SPACING = 60
MINIMUM_DIFFICULTY_GAP = 121
NORMAL_GAP = 1

ALGO_IDS = {"sha256d": 0, "scrypt": 1, "groestl": 2, "skein": 3, "qubit": 4, "odo": 7}

# The five algorithms available between the MultiAlgo height and the Odocrypt
# height, used in rotation.
MULTIALGO_ROTATION = ["sha256d", "scrypt", "groestl", "skein", "qubit"]

# Blocks that are given a large time gap, and the algorithm each one is mined
# with. These are what the lookup has to step past.
FORCED_BLOCKS = {
    # one on its own, and a run of three of the same algorithm
    205: "qubit",
    210: "scrypt", 211: "scrypt", 212: "scrypt",
    # spread through the chain
    240: "sha256d", 280: "skein", 320: "groestl", 360: "scrypt",
    # a run of three of one algorithm across the height where the rules change
    399: "skein", 400: "skein", 401: "skein",
    450: "qubit", 500: "sha256d", 550: "scrypt",
    # three in a row of different algorithms across the Odocrypt height
    598: "skein", 599: "qubit", 600: "scrypt",
    # every Odocrypt block, so no Odocrypt block is left for the lookup to find
    601: "odo", 602: "odo", 603: "odo", 604: "odo", 605: "odo",
}

# The last blocks use only these, so that the most recent scrypt, qubit and
# Odocrypt blocks are all ones the lookup has to step past.
TAIL_ROTATION = ["sha256d", "skein"]


class DigiByteDifficultyLookupTest(DigiByteTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True
        # A fixed regtest address, so the test needs no wallet.
        self.mining_address = "dgbrt1qtmp74ayg7p24uslctssvjm06q5phz4yrgndnyh"

    def skip_test_if_missing_module(self):
        pass

    def block_plan(self):
        """Which algorithm mines each block above the MultiAlgo height, and how many
        seconds after its parent it is stamped."""
        plan = []
        for height in range(MULTIALGO_HEIGHT + 1, TIP_HEIGHT + 1):
            if height in FORCED_BLOCKS:
                plan.append((height, FORCED_BLOCKS[height], MINIMUM_DIFFICULTY_GAP))
            elif height > ODOCRYPT_HEIGHT:
                # Groestl is switched off above the Odocrypt height.
                plan.append((height, TAIL_ROTATION[height % len(TAIL_ROTATION)], NORMAL_GAP))
            else:
                plan.append((height, MULTIALGO_ROTATION[height % len(MULTIALGO_ROTATION)], NORMAL_GAP))
        return plan

    def mine_the_chain(self):
        node = self.nodes[0]

        self.log.info("Mining %d scrypt blocks, which is the only algorithm the chain allows up to height %d",
                      MULTIALGO_HEIGHT, MULTIALGO_HEIGHT)
        self.generatetoaddress(node, MULTIALGO_HEIGHT, self.mining_address, 1000000, "scrypt",
                               sync_fun=self.no_op)
        assert_equal(node.getblockcount(), MULTIALGO_HEIGHT)

        # From here the test sets the node's clock itself, so it decides exactly
        # which blocks count as mined at the minimum difficulty.
        stamp = node.getblockheader(node.getbestblockhash())["time"]
        plan = self.block_plan()
        self.log.info("Mining blocks %d to %d with five algorithms, %d of them at the minimum difficulty",
                      MULTIALGO_HEIGHT + 1, TIP_HEIGHT, len(FORCED_BLOCKS))
        for height, algo, gap in plan:
            stamp += gap
            node.setmocktime(stamp)
            self.generatetoaddress(node, 1, self.mining_address, 1000000, algo, sync_fun=self.no_op)
            assert_equal(node.getblockcount(), height)

        assert_equal(node.getblockcount(), TIP_HEIGHT)
        self.log.info("The last block is stamped %d seconds after the last one mined on the real clock",
                      stamp - node.getblockheader(node.getblockhash(MULTIALGO_HEIGHT))["time"])

    def read_the_headers(self, tip_height=TIP_HEIGHT):
        """Every block header, by height, read back from the node."""
        node = self.nodes[0]
        headers = {}
        header = node.getblockheader(node.getbestblockhash())
        while True:
            headers[header["height"]] = header
            if "previousblockhash" not in header:
                break
            header = node.getblockheader(header["previousblockhash"])
        assert_equal(len(headers), tip_height + 1)
        return headers

    def block_the_rule_picks(self, headers, from_height, algo_id):
        """The most recent block at or before this one that was mined with this
        algorithm and was not stamped more than two block spacings after its parent."""
        height = from_height
        while height >= 0:
            header = headers[height]
            if header["pow_algo_id"] == algo_id:
                minimum_difficulty = height > 0 and \
                    header["time"] > headers[height - 1]["time"] + 2 * TARGET_SPACING
                if not minimum_difficulty:
                    return header
            height -= 1
        return None

    def check_the_chain_was_accepted(self, headers):
        node = self.nodes[0]
        assert_equal(node.getblockcount(), TIP_HEIGHT)
        tips = node.getchaintips()
        assert_equal(len(tips), 1)
        assert_equal(tips[0]["status"], "active")
        assert_equal(tips[0]["height"], TIP_HEIGHT)

        # The blocks the test asked to be mined at the minimum difficulty really
        # are, and enough algorithms are covered.
        covered = set()
        for height, algo in FORCED_BLOCKS.items():
            header = headers[height]
            assert_equal(header["pow_algo_id"], ALGO_IDS[algo])
            assert header["time"] > headers[height - 1]["time"] + 2 * TARGET_SPACING, \
                f"block {height} was meant to be mined at the minimum difficulty"
            covered.add(algo)
        assert len(covered) >= 5, f"only these algorithms have minimum-difficulty blocks: {covered}"

        # For several algorithms the lookup has to step past the most recent block
        # of that algorithm, which is what makes this chain worth mining.
        stepped_past = []
        for algo, algo_id in ALGO_IDS.items():
            latest = None
            for height in range(TIP_HEIGHT, -1, -1):
                if headers[height]["pow_algo_id"] == algo_id:
                    latest = height
                    break
            if latest is None:
                continue
            picked = self.block_the_rule_picks(headers, TIP_HEIGHT, algo_id)
            if picked is None or picked["height"] != latest:
                stepped_past.append(algo)
        assert len(stepped_past) >= 3, \
            f"the lookup only has to step past a block for these algorithms: {stepped_past}"
        self.log.info("At the tip the lookup steps past the most recent block of: %s",
                      ", ".join(sorted(stepped_past)))

        # No Odocrypt block is left for the lookup to find, so the node has to
        # fall back to the starting difficulty for that algorithm.
        assert_equal(self.block_the_rule_picks(headers, TIP_HEIGHT, ALGO_IDS["odo"]), None)

    def check_the_reported_difficulties(self, headers, what, tip_height=TIP_HEIGHT):
        node = self.nodes[0]
        reported = node.getdifficulty()["difficulties"]

        # Groestl is switched off at the Odocrypt height and Odocrypt is switched
        # on, so these are the five the node reports at this tip.
        assert_equal(sorted(reported.keys()), sorted(["sha256d", "scrypt", "skein", "qubit", "odo"]))

        for algo, difficulty in reported.items():
            picked = self.block_the_rule_picks(headers, tip_height, ALGO_IDS[algo])
            if picked is None:
                # The node reports the starting difficulty of the algorithm, which
                # on regtest is the easiest the network allows.
                expected = node.getblockheader(node.getblockhash(0))["difficulty"]
            else:
                expected = picked["difficulty"]
            assert_equal(difficulty, expected)
            assert difficulty > 0
        self.log.info("%s: the reported difficulties come from the blocks the rule picks", what)
        return reported

    def run_test(self):
        node = self.nodes[0]
        self.mine_the_chain()

        headers = self.read_the_headers()
        self.check_the_chain_was_accepted(headers)
        first = self.check_the_reported_difficulties(headers, "freshly mined")
        tip = node.getbestblockhash()
        tip_stamp = headers[TIP_HEIGHT]["time"]

        self.log.info("Restarting the node")
        self.restart_node(0)
        assert_equal(node.getbestblockhash(), tip)
        assert_equal(self.check_the_reported_difficulties(headers, "after a restart"), first)

        self.log.info("Reindexing the whole chain from the block files")
        self.restart_node(0, extra_args=["-reindex"])
        assert_equal(node.getbestblockhash(), tip)
        assert_equal(node.getblockcount(), TIP_HEIGHT)
        assert_equal(self.check_the_reported_difficulties(headers, "after a reindex"), first)

        # One more block of each algorithm that is switched on. Every one has to
        # be accepted, which means the difficulty the miner used and the
        # difficulty validation expected still agree after all those
        # minimum-difficulty blocks, and the reported difficulty has to follow
        # the new blocks.
        self.log.info("Mining one more block with each algorithm that is switched on")
        stamp = tip_stamp
        for algo in ["sha256d", "scrypt", "skein", "qubit", "odo"]:
            stamp += NORMAL_GAP
            node.setmocktime(stamp)
            self.generatetoaddress(node, 1, self.mining_address, 1000000, algo, sync_fun=self.no_op)
        assert_equal(node.getblockcount(), TIP_HEIGHT + 5)

        headers = self.read_the_headers(TIP_HEIGHT + 5)
        self.check_the_reported_difficulties(headers, "after five more blocks", TIP_HEIGHT + 5)


if __name__ == '__main__':
    DigiByteDifficultyLookupTest().main()
