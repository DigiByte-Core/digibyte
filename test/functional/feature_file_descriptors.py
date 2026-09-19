#!/usr/bin/env python3
# Copyright (c) 2026 The DigiByte Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Reserve descriptors for buffered databases before assigning peer connections."""

from contextlib import contextmanager
import os
import sys

if os.name == "posix":
    import resource

from test_framework.test_framework import DigiByteTestFramework, SkipTest
from test_framework.util import assert_equal


class FileDescriptorTest(DigiByteTestFramework):
    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 1
        self.wallet_names = []

    def skip_test_if_missing_module(self):
        if os.name != "posix":
            raise SkipTest("This test requires POSIX descriptor limits")
        self.parent_limits = resource.getrlimit(resource.RLIMIT_NOFILE)
        hard_limit = self.parent_limits[1]
        if hard_limit != resource.RLIM_INFINITY and hard_limit < 512:
            raise SkipTest("This test requires a hard descriptor limit of at least 512")

    def setup_network(self):
        self.add_nodes(self.num_nodes)

    @contextmanager
    def child_limits(self, soft_limit, hard_limit):
        node = self.nodes[0]
        original_args = node.args
        # Exec a small child wrapper instead of changing limits in the test runner.
        wrapper = (
            "import os, resource, sys\n"
            "resource.setrlimit(resource.RLIMIT_NOFILE, (int(sys.argv[1]), int(sys.argv[2])))\n"
            "os.execvp(sys.argv[3], sys.argv[3:])\n"
        )
        node.args = [sys.executable, "-c", wrapper, str(soft_limit), str(hard_limit), *original_args]
        try:
            yield
        finally:
            node.args = original_args
            assert_equal(resource.getrlimit(resource.RLIMIT_NOFILE), self.parent_limits)

    def run_test(self):
        node = self.nodes[0]
        self.log.info("Refuse hard limits below the database and network descriptor reserve")
        for limit in (256, 341, 342, 350, 351):
            with self.child_limits(limit, limit):
                node.assert_start_raises_init_error(
                    # If the refusal regresses, exit after startup instead of waiting for RPC.
                    extra_args=["-maxconnections=10000", "-stopafterblockimport=1"],
                    expected_msg="Error: Not enough file descriptors available.",
                )

        # The failed starts exit before logging is initialized.
        node.debug_log_path.parent.mkdir(parents=True, exist_ok=True)
        node.debug_log_path.touch()

        self.log.info("Clamp connections without taking the database descriptor reserve")
        cases = [(352, 0), (512, 160)]
        if self.parent_limits[1] == resource.RLIM_INFINITY or self.parent_limits[1] >= 1024:
            cases.append((1024, 672))
        for limit, connections in cases:
            warning = f"Warning: Reducing -maxconnections from 10000 to {connections}, because of system limitations."
            with self.child_limits(limit, limit):
                with node.assert_debug_log([f"Using at most {connections} automatic connections ({limit} file descriptors available)"]):
                    self.start_node(0, extra_args=["-maxconnections=10000"])
                assert_equal(node.getblockcount(), 0)
                self.stop_node(0, expected_stderr=warning)

        self.log.info("Clamp large connection requests without overflowing the descriptor total")
        for requested_connections in (2**31 - 1 - 256, 2**31 - 1):
            warning = f"Warning: Reducing -maxconnections from {requested_connections} to 160, because of system limitations."
            with self.child_limits(512, 512):
                with node.assert_debug_log(["Using at most 160 automatic connections (512 file descriptors available)"]):
                    self.start_node(0, extra_args=[f"-maxconnections={requested_connections}"])
                assert_equal(node.getblockcount(), 0)
                self.stop_node(0, expected_stderr=warning)

        self.log.info("Raise a low soft limit when the hard limit permits the requested connections")
        with self.child_limits(256, 512):
            with node.assert_debug_log(["Using at most 125 automatic connections (477 file descriptors available)"]):
                self.start_node(0, extra_args=["-maxconnections=125"])
            assert_equal(node.getblockcount(), 0)
            self.stop_node(0)


if __name__ == "__main__":
    FileDescriptorTest().main()
