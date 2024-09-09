# Copyright 2024 Memgraph Ltd.
#
# Use of this software is governed by the Business Source License
# included in the file licenses/BSL.txt; by using this file, you agree to be bound by the terms of the Business Source
# License, and you may not use this file except in compliance with the Business Source License.
#
# As of the Change Date specified in that file, in accordance with
# the Business Source License, use of this software will be governed
# by the Apache License, Version 2.0, included in the file
# licenses/APL.txt.

import os
import shutil
import tempfile

import interactive_mg_runner
import pytest
from common import execute_and_fetch_all
from mg_utils import mg_sleep_and_assert, mg_sleep_and_assert_collection
from multitenancy_common import (
    BOLT_PORTS,
    MEMGRAPH_INSTANCES_DESCRIPTION_WITH_RECOVERY,
    REPLICATION_PORTS,
    TEMP_DIR,
    create_memgraph_instances_with_role_recovery,
    do_manual_setting_up,
    get_number_of_edges_func,
    get_number_of_nodes_func,
    safe_execute,
    setup_main,
    setup_replication,
    show_databases_func,
    show_replicas_func,
)

interactive_mg_runner.SCRIPT_DIR = os.path.dirname(os.path.realpath(__file__))
interactive_mg_runner.PROJECT_DIR = os.path.normpath(
    os.path.join(interactive_mg_runner.SCRIPT_DIR, "..", "..", "..", "..")
)
interactive_mg_runner.BUILD_DIR = os.path.normpath(os.path.join(interactive_mg_runner.PROJECT_DIR, "build"))
interactive_mg_runner.MEMGRAPH_BINARY = os.path.normpath(os.path.join(interactive_mg_runner.BUILD_DIR, "memgraph"))


@pytest.mark.parametrize("replica_name", [("replica_1"), ("replica_2")])
def test_multitenancy_replication_drop_replica(connection, replica_name):
    # Goal: show that the cluster can recover if a replica is dropped and registered again
    # 0/ Setup replication
    # 1/ MAIN CREATE DATABASE A and B
    # 2/ Write on MAIN to A and B
    # 3/ Drop and add the same replica
    # 4/ Validate data on replica

    # 0/
    data_directory = tempfile.TemporaryDirectory()
    MEMGRAPH_INSTANCES_DESCRIPTION = create_memgraph_instances_with_role_recovery(data_directory.name)

    interactive_mg_runner.start_all(MEMGRAPH_INSTANCES_DESCRIPTION)
    do_manual_setting_up(connection)
    main_cursor = connection(BOLT_PORTS["main"], "main").cursor()

    # 1/
    execute_and_fetch_all(main_cursor, "CREATE DATABASE A;")
    execute_and_fetch_all(main_cursor, "CREATE DATABASE B;")

    # 2/
    setup_main(main_cursor)

    # 3/
    execute_and_fetch_all(main_cursor, f"DROP REPLICA {replica_name};")
    sync = {"replica_1": "SYNC", "replica_2": "ASYNC"}
    execute_and_fetch_all(
        main_cursor,
        f"REGISTER REPLICA  {replica_name} {sync[replica_name]} TO '127.0.0.1:{REPLICATION_PORTS[replica_name]}';",
    )

    # 4/
    expected_data = [
        (
            "replica_1",
            f"127.0.0.1:{REPLICATION_PORTS['replica_1']}",
            "sync",
            {
                "A": {"ts": 7, "behind": 0, "status": "ready"},
                "B": {"ts": 3, "behind": 0, "status": "ready"},
                "memgraph": {"ts": 0, "behind": 0, "status": "ready"},
            },
        ),
        (
            "replica_2",
            f"127.0.0.1:{REPLICATION_PORTS['replica_2']}",
            "async",
            {
                "A": {"ts": 7, "behind": 0, "status": "ready"},
                "B": {"ts": 3, "behind": 0, "status": "ready"},
                "memgraph": {"ts": 0, "behind": 0, "status": "ready"},
            },
        ),
    ]

    cursor_replica = connection(BOLT_PORTS[replica_name], "replica").cursor()
    assert get_number_of_nodes_func(cursor_replica, "A")() == 7
    assert get_number_of_edges_func(cursor_replica, "A")() == 3
    assert get_number_of_nodes_func(cursor_replica, "B")() == 2
    assert get_number_of_edges_func(cursor_replica, "B")() == 0


def test_multitenancy_replication_restart_main(connection):
    # Goal: show that the cluster can restore to a correct state if the MAIN restarts
    # 0/ Setup replication
    # 1/ MAIN CREATE DATABASE A and B
    # 2/ Write on MAIN to A and B
    # 3/ Restart main and write new data
    # 4/ Validate data on replica

    # 0/
    # Tmp dir should already be removed, but sometimes its not...
    safe_execute(shutil.rmtree, TEMP_DIR)
    interactive_mg_runner.start_all(MEMGRAPH_INSTANCES_DESCRIPTION_WITH_RECOVERY)
    setup_replication(connection)
    main_cursor = connection(BOLT_PORTS["main"], "main").cursor()

    # 1/
    execute_and_fetch_all(main_cursor, "CREATE DATABASE A;")
    execute_and_fetch_all(main_cursor, "CREATE DATABASE B;")

    # 2/
    setup_main(main_cursor)

    # 3/
    interactive_mg_runner.kill(MEMGRAPH_INSTANCES_DESCRIPTION_WITH_RECOVERY, "main")
    interactive_mg_runner.start(MEMGRAPH_INSTANCES_DESCRIPTION_WITH_RECOVERY, "main")
    main_cursor = connection(BOLT_PORTS["main"], "main").cursor()

    execute_and_fetch_all(main_cursor, "USE DATABASE A;")
    execute_and_fetch_all(main_cursor, "CREATE (:Node{on:'A'});")
    execute_and_fetch_all(main_cursor, "USE DATABASE B;")
    execute_and_fetch_all(main_cursor, "CREATE (:Node{on:'B'});")

    # 4/
    cursor_replica = connection(BOLT_PORTS["replica_1"], "replica").cursor()
    execute_and_fetch_all(cursor_replica, "USE DATABASE A;")
    assert get_number_of_nodes_func(cursor_replica, "A")() == 8
    assert get_number_of_edges_func(cursor_replica, "A")() == 3
    assert get_number_of_nodes_func(cursor_replica, "B")() == 3
    assert get_number_of_edges_func(cursor_replica, "B")() == 0

    cursor_replica = connection(BOLT_PORTS["replica_2"], "replica").cursor()
    execute_and_fetch_all(cursor_replica, "USE DATABASE A;")
    assert get_number_of_nodes_func(cursor_replica, "A")() == 8
    assert get_number_of_edges_func(cursor_replica, "A")() == 3
    assert get_number_of_nodes_func(cursor_replica, "B")() == 3
    assert get_number_of_edges_func(cursor_replica, "B")() == 0


def test_automatic_databases_drop_multitenancy_replication(connection):
    # Goal: show that drop database can be replicated
    # 0/ Setup replication
    # 1/ MAIN CREATE DATABASE A
    # 2/ Write to MAIN A
    # 3/ Validate replication of changes to A have arrived at REPLICA
    # 4/ DROP DATABASE A/B
    # 5/ Check that the drop replicated

    # 0/
    data_directory = tempfile.TemporaryDirectory()
    MEMGRAPH_INSTANCES_DESCRIPTION = create_memgraph_instances_with_role_recovery(data_directory.name)
    interactive_mg_runner.start_all(MEMGRAPH_INSTANCES_DESCRIPTION)

    do_manual_setting_up(connection)

    main_cursor = connection(BOLT_PORTS["main"], "main").cursor()

    # 1/
    execute_and_fetch_all(main_cursor, "CREATE DATABASE A;")
    execute_and_fetch_all(main_cursor, "CREATE DATABASE B;")

    # 2/
    execute_and_fetch_all(main_cursor, "USE DATABASE A;")
    execute_and_fetch_all(main_cursor, "CREATE (:Node{on:'A'});")

    # 3/
    expected_data = [
        (
            "replica_1",
            f"127.0.0.1:{REPLICATION_PORTS['replica_1']}",
            "sync",
            {"ts": 4, "behind": None, "status": "ready"},
            {
                "A": {"ts": 1, "behind": 0, "status": "ready"},
                "B": {"ts": 0, "behind": 0, "status": "ready"},
                "memgraph": {"ts": 0, "behind": 0, "status": "ready"},
            },
        ),
        (
            "replica_2",
            f"127.0.0.1:{REPLICATION_PORTS['replica_2']}",
            "async",
            {"ts": 4, "behind": None, "status": "ready"},
            {
                "A": {"ts": 1, "behind": 0, "status": "ready"},
                "B": {"ts": 0, "behind": 0, "status": "ready"},
                "memgraph": {"ts": 0, "behind": 0, "status": "ready"},
            },
        ),
    ]
    mg_sleep_and_assert(expected_data, show_replicas_func(main_cursor))

    # 4/
    execute_and_fetch_all(main_cursor, "USE DATABASE memgraph;")
    execute_and_fetch_all(main_cursor, "DROP DATABASE A;")
    execute_and_fetch_all(main_cursor, "DROP DATABASE B;")

    # 5/
    databases_on_main = show_databases_func(main_cursor)()

    replica_cursor = connection(BOLT_PORTS["replica_1"], "replica").cursor()
    mg_sleep_and_assert(databases_on_main, show_databases_func(replica_cursor))

    replica_cursor = connection(BOLT_PORTS["replica_2"], "replica").cursor()
    mg_sleep_and_assert(databases_on_main, show_databases_func(replica_cursor))