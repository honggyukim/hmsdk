#!/usr/bin/env python3
# Copyright (c) 2023-2024 SK hynix, Inc.
# SPDX-License-Identifier: BSD 2-Clause

"""Common utilities for policy generation scripts."""

import argparse
import os
import subprocess as sp

# Common options
monitoring_intervals = "--monitoring_intervals 100ms 2s 20s"
monitoring_nr_regions_range = "--monitoring_nr_regions_range 100 10000"

# Common DAMOS options
damos_sz_region = "--damos_sz_region 4096 max"
damos_filter = "--damos_filter memcg nomatching /hmsdk"


def parse_argument():
    """Return parsed command line arguments."""
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "-d",
        "--demote",
        dest="migrate_cold",
        action="append",
        nargs=2,
        metavar=("SRC", "DEST"),
        default=[],
        help="source and destination NUMA nodes for demotion.",
    )
    parser.add_argument(
        "-p",
        "--promote",
        dest="migrate_hot",
        action="append",
        nargs=2,
        metavar=("SRC", "DEST"),
        default=[],
        help="source and destination NUMA nodes for promotion.",
    )
    parser.add_argument(
        "-g",
        "--global",
        dest="nofilter",
        action="store_true",
        help="Apply tiered migration schemes globally not limited to PIDs under 'hmsdk' cgroup.",
    )
    parser.add_argument(
        "-o",
        "--output",
        dest="output",
        default=None,
        help="Set the output json file.",
    )
    return parser.parse_args()


def run_command(cmd: str) -> str:
    """Run the given command and return its stdout."""
    with sp.Popen(cmd.split(), stdout=sp.PIPE, stderr=sp.PIPE) as proc:
        stdout, stderr = proc.communicate()
        if stderr:
            print(stderr.decode(errors="ignore"))
        return stdout.decode(errors="ignore")


def parent_dir_of_file(file: str) -> str:
    """Return parent directory two levels up from the given file path."""
    return os.path.dirname(os.path.dirname(os.path.abspath(file)))


class CheckNodes:
    """Validate command line node arguments."""

    def __init__(self):
        self.handled_node = set()

    def __call__(self, src_node: str, dest_node: str, node_json: dict):
        if src_node in self.handled_node:
            return f"node {src_node} cannot be used multiple times for source node"
        self.handled_node.add(src_node)

        if src_node == dest_node:
            return f"node {src_node} cannot be used for both SRC and DEST node"

        nr_regions = len(node_json["kdamonds"][0]["contexts"][0]["targets"][0]["regions"])
        if nr_regions <= 0:
            return f"node {src_node} has no valid regions"

        return None
