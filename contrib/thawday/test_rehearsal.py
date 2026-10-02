#!/usr/bin/env python3
"""Check rehearsal control flow without starting nodes or changing a chain."""

import unittest
from unittest.mock import patch

import rehearsal


class QuoteWaitTests(unittest.TestCase):
    def make_lab(self, *, ready_at=12, reported_epoch=124, bundle_epoch=None, price=45000):
        clock = {"now": 0}
        lab = object.__new__(rehearsal.Lab)
        lab.miner_started_at = 4800
        state = {"height": 4988, "signed": None, "mined": []}

        class Node:
            def rpc(self, method):
                if method == "getblockcount":
                    return state["height"]
                if method == "getprotectionstatus":
                    return {"volatility": {
                        "quote_available": True,
                        "candidate_price_micro_usd": price,
                    }}
                if method == "getdigidollardeploymentinfo":
                    return {"musig2_session": {
                        "epoch": reported_epoch,
                        "state": "complete" if clock["now"] >= ready_at else "signing",
                    }}
                raise AssertionError(method)

        lab.nodes = [Node(), Node(), Node()]
        lab.note = lambda *args: None
        lab.signed_bundle = lambda height: state["signed"]

        def mine(count, ceiling=None):
            self.assertEqual(count, 1)
            state["height"] += 1
            if ceiling is not None:
                self.assertLessEqual(state["height"], ceiling)
            state["mined"].append((state["height"], clock["now"]))
            state["signed"] = None
            if clock["now"] >= ready_at:
                state["signed"] = {
                    "price_micro_usd": price,
                    "epoch": reported_epoch if bundle_epoch is None else bundle_epoch,
                    "height": state["height"],
                    "signers": list(range(7)),
                }

        lab.mine = mine
        return lab, state, clock

    def wait(self, lab, clock, ceiling):
        def sleep(seconds):
            clock["now"] += seconds

        with patch.object(rehearsal.time, "monotonic", lambda: clock["now"]), \
                patch.object(rehearsal.time, "sleep", sleep):
            lab.await_quote(45000, ceiling)

    def test_reserves_last_block_until_signatures_are_ready(self):
        lab, state, clock = self.make_lab()
        self.wait(lab, clock, 4990)
        self.assertEqual(state["height"], 4990)
        self.assertEqual(len(state["mined"]), 2)
        self.assertGreaterEqual(state["mined"][-1][1], 12)
        self.assertEqual(state["signed"]["price_micro_usd"], 45000)

    def test_completed_old_epoch_cannot_spend_the_last_block(self):
        lab, state, clock = self.make_lab(ready_at=0, reported_epoch=123)
        with self.assertRaises(rehearsal.Failure):
            self.wait(lab, clock, 4990)
        self.assertEqual(state["height"], 4989)

    def test_reports_exhausted_height_instead_of_waiting_for_an_old_block(self):
        lab, state, clock = self.make_lab()
        state["height"] = 4990
        with self.assertRaisesRegex(rehearsal.Failure, "height limit"):
            self.wait(lab, clock, 4990)
        self.assertEqual(clock["now"], 0)
        self.assertEqual(state["mined"], [])

    def test_accepts_a_valid_quote_already_at_the_height_limit(self):
        lab, state, clock = self.make_lab(ready_at=0)
        state["height"] = 4989
        lab.mine(1, ceiling=4990)
        self.wait(lab, clock, 4990)
        self.assertEqual(len(state["mined"]), 1)

    def test_can_mine_a_ready_bundle_at_the_next_epoch_boundary(self):
        # The status RPC describes the tip's round, even when the next round
        # has already signed a bundle for the boundary block.
        lab, state, clock = self.make_lab(ready_at=0, reported_epoch=124, bundle_epoch=125)
        state["height"] = 4999
        self.wait(lab, clock, 5000)
        self.assertEqual(state["height"], 5000)
        self.assertEqual(state["signed"]["epoch"], 125)

    def test_no_height_limit_still_allows_progress_to_a_signed_block(self):
        lab, state, clock = self.make_lab()
        self.wait(lab, clock, None)
        self.assertGreaterEqual(state["mined"][-1][1], 12)
        self.assertIsNotNone(state["signed"])

    def test_wrong_price_is_never_accepted(self):
        lab, state, clock = self.make_lab(ready_at=0, price=30000)
        with self.assertRaises(rehearsal.Failure):
            self.wait(lab, clock, 4990)


if __name__ == "__main__":
    unittest.main()
