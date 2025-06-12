#!/usr/bin/env python3
# Copyright (c) 2023-2024 SK hynix, Inc.
# SPDX-License-Identifier: BSD 2-Clause

import json
import os
import sys

import yaml

from gen_common import (
    CheckNodes,
    monitoring_intervals,
    monitoring_nr_regions_range,
    damos_sz_region,
    damos_filter,
    parse_argument,
    parent_dir_of_file,
    run_command,
)




def main():
    args = parse_argument()

    if os.geteuid() != 0:
        print("error: Run as root")
        sys.exit(1)

    damo = parent_dir_of_file(__file__) + "/damo/damo"
    node_jsons = []

    common_opts = f"{monitoring_intervals} {monitoring_nr_regions_range}"
    common_damos_opts = f"{damos_sz_region}"
    if not args.nofilter:
        common_damos_opts += f" {damos_filter}"

    check_nodes = CheckNodes()

    for src_node, dest_node in args.migrate_cold:
        numa_node = f"--numa_node {src_node}"
        damos_action = f"--damos_action migrate_cold {dest_node}"
        damos_access_rate = "--damos_access_rate 0% 0%"
        damos_age = "--damos_age 30s max"
        damos_quotas = "--damos_quotas 1s 50G 20s 0 0 1%"
        damos_young_filter = "--damos_filter young matching"
        cmd = (
            f"{damo} args damon --format json {numa_node} {common_opts} "
            f"{damos_action} {common_damos_opts} {damos_young_filter} "
            f"{damos_access_rate} {damos_age} {damos_quotas}"
        )
        json_str = run_command(cmd)
        node_json = json.loads(json_str)
        node_jsons.append(node_json)
        err = check_nodes(src_node, dest_node, node_json)
        if err:
            print(f"error: {err}")
            sys.exit(1)

    for src_node, dest_node in args.migrate_hot:
        numa_node = f"--numa_node {src_node}"
        damos_action = f"--damos_action migrate_hot {dest_node}"
        damos_access_rate = "--damos_access_rate 5% 100%"
        damos_age = "--damos_age 0 max"
        damos_quotas = "--damos_quotas 2s 50G 20s 0 0 1%"
        damos_young_filter = "--damos_filter young nomatching"
        cmd = (
            f"{damo} args damon --format json {numa_node} {common_opts} "
            f"{damos_action} {common_damos_opts} {damos_young_filter} "
            f"{damos_access_rate} {damos_age} {damos_quotas}"
        )
        json_str = run_command(cmd)
        node_json = json.loads(json_str)
        node_jsons.append(node_json)
        err = check_nodes(src_node, dest_node, node_json)
        if err:
            print(f"error: {err}")
            sys.exit(1)

    nodes = {"kdamonds": []}

    for node_json in node_jsons:
        nodes["kdamonds"].append(node_json["kdamonds"][0])

    config = yaml.dump(nodes, default_flow_style=False, sort_keys=False)
    if args.output:
        with open(args.output, "w") as f:
            f.write(config + "\n")
    else:
        print(config)

    return 0


if __name__ == "__main__":
    sys.exit(main())
