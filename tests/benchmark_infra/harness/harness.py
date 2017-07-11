#!/usr/bin/env python3
# -*- coding: utf-8 -*-

import logging
import os
from os import path
import requests
import time
import itertools
import json
from subprocess import check_output
from argparse import ArgumentParser
from collections import OrderedDict
from collections import defaultdict
import tempfile

import jail_faker as jail
from bolt_client import WALL_TIME
from perf import Perf

log = logging.getLogger(__name__)


class QuerySuite():
    """
    Executes a Query-based benchmark scenario. Query-based scenarios
    consist of setup steps (Cypher queries) executed before the benchmark,
    a single Cypher query that is benchmarked, and teardown steps
    (Cypher queries) executed after the benchmark.
    """
    # what the QuerySuite can work with
    KNOWN_KEYS = {"config", "setup", "itersetup", "run", "iterteardown",
                  "teardown"}
    summary = "Summary:\n{:>30}{:>30}{:>30}{:>30}{:>30}\n".format(
            "scenario_name", "query_parsing_time", "query_planning_time",
            "query_plan_execution_time", WALL_TIME)

    def __init__(self, args):
        self.perf = Perf()
        argp = ArgumentParser(description=__doc__)
        argp.add_argument("--perf", help="Run perf on memgraph binary.",
                          action="store_true")
        args, _ = argp.parse_known_args(args)
        self.perf = Perf() if args.perf else None

    class Loader():
        """
        Loads file contents. Supported types are:
            .py - executable that prints out Cypher queries
            .cypher - contains Cypher queries in textual form
            .json - contains a configuration

        A QueryLoader object is callable.
        A call to it returns a generator that yields loaded data
        (Cypher queries, configuration). In that sense one
        QueryLoader is reusable. The generator approach makes it possible
        to generated different queries each time when executing a .py file.
        """
        def __init__(self, file_path):
            self.file_path = file_path

        def _queries(self, data):
            """ Helper function for breaking down and filtering queries"""
            for element in filter(
                    None, map(str.strip, data.replace("\n", " ").split(";"))):
                yield element

        def __call__(self):
            """ Yields queries found in the given file_path one by one """
            log.debug("Generating queries from file_path: %s",
                      self.file_path)
            _, extension = path.splitext(self.file_path)
            if extension == ".cypher":
                with open(self.file_path) as f:
                    return self._queries(f.read())
            elif extension == ".py":
                return self._queries(check_output(
                    ["python3", self.file_path]).decode("ascii"))
            elif extension == ".json":
                with open(self.file_path) as f:
                    return [json.load(f)].__iter__()
            else:
                raise Exception("Unsupported filetype {} ".format(extension))

        def __repr__(self):
            return "(QuerySuite.Loader<%s>)" % self.file_path

    @staticmethod
    def scenarios(args):
        """
        Scans through folder structure starting with groups_root and
        loads query scenarios.
        Expected folder structure is:
            groups_root/
                groupname1/
                    config.json
                    setup.FILE_TYPE
                    teardown.FILE_TYPE
                    itersetup.FILE_TYPE
                    iterteardown.FILE_TYPE
                    scenario1.config.json
                    scenario1.run.FILE_TYPE-------(mandatory)
                    scenario1.setup.FILE_TYPE
                    scenario1.teardown.FILE_TYPE
                    scenario1.itersetup.FILE_TYPE
                    scenario1.iterteardown.FILE_TYPE
                    scenario2...
                                ...
                groupname2/
                            ...

        Per query configs (setup, teardown, itersetup, iterteardown)
        override group configs for that scenario. Group configs must have one
        extension (.FILE_TYPE) and
        scenario configs must have 2 extensions (.scenario_name.FILE_TYPE).
        See `QueryLoader` documentation to see which file types are supported.

        Args:
            args: additional args parsed by this function
            group_paths: str, root folder that contains group folders
        Return:
            {group: (scenario, {config: query_generator_function})
        """
        argp = ArgumentParser("QuerySuite.scenarios argument parser")
        argp.add_argument("--query-scenarios-root", default=path.join(
            path.dirname(path.dirname(path.realpath(__file__))), "groups"),
            dest="root")
        args, _ = argp.parse_known_args()
        log.info("Loading query scenarios from root: %s", args.root)

        def fill_config_dict(config_dict, base, config_files):
            for config_file in config_files:
                log.debug("Processing config file %s", config_file)
                config_name = config_file.split(".")[-2]
                config_dict[config_name] = QuerySuite.Loader(
                    path.join(base, config_file))

            # validate that the scenario does not contain any illegal
            # keys (defense against typos in file naming)
            unknown_keys = set(config_dict) - QuerySuite.KNOWN_KEYS
            if unknown_keys:
                raise Exception("Unknown QuerySuite config elements: '%r'" %
                                unknown_keys)

        def dir_content(root, predicate):
            return [p for p in os.listdir(root)
                    if predicate(path.join(root, p))]

        group_scenarios = OrderedDict()
        for group in dir_content(args.root, path.isdir):
            log.info("Loading group: '%s'", group)

            group_scenarios[group] = []
            files = dir_content(path.join(args.root, group),
                                path.isfile)

            # process group default config
            group_config = {}
            fill_config_dict(group_config, path.join(args.root, group),
                             [f for f in files if f.count(".") == 1])

            # group files on scenario
            for scenario_name, scenario_files in itertools.groupby(
                    filter(lambda f: f.count(".") == 2, sorted(files)),
                    lambda x: x.split(".")[0]):
                log.info("Loading scenario: '%s'", scenario_name)
                scenario = dict(group_config)
                fill_config_dict(scenario,
                                 path.join(args.root, group),
                                 scenario_files)
                group_scenarios[group].append((scenario_name, scenario))
                log.debug("Loaded config for scenario '%s'\n%r", scenario_name,
                          scenario)

        return group_scenarios

    def run(self, scenario, scenario_name, runner):
        log.debug("QuerySuite.run() with scenario: %s", scenario)
        scenario_config = scenario.get("config")
        scenario_config = next(scenario_config()) if scenario_config else {}

        def execute(config_name):
            queries = scenario.get(config_name)
            return runner.execute(queries()) if queries else None

        measurements = []

        measurement_sums = defaultdict(float)
        def add_measurement(dictionary, iteration, key):
            if key in dictionary:
                measurement = {"target": key, "value": dictionary[key],
                               "unit": "s", "type": "time"}
                measurement["iteration"] = iteration
                measurements.append(measurement)
                try:
                    measurement_sums[key] += float(dictionary[key])
                except:
                    pass

        pid = runner.start()
        execute("setup")

        # warmup phase
        for _ in range(min(scenario_config.get("iterations", 1),
                           scenario_config.get("warmup", 3))):
            execute("itersetup")
            execute("run")
            execute("iterteardown")

        if self.perf:
            self.perf.start(pid)

        # TODO per scenario/run runner configuration
        num_iterations = scenario_config.get("iterations", 1)
        for iteration in range(num_iterations):
            # TODO if we didn't have the itersetup it would be trivial
            # to move iteration to the bolt_client script, so we would not
            # have to start and stop the client for each iteration, it would
            # most likely run faster
            execute("itersetup")
            # TODO measure CPU time (expose it from the runner)
            run_result = execute("run")
            assert len(run_result.get("metadatas", [])), \
                "Scenario run must have exactly one query"
            add_measurement(run_result, iteration, WALL_TIME)
            add_measurement(run_result["metadatas"][0], iteration,
                            "query_parsing_time")
            add_measurement(run_result["metadatas"][0], iteration,
                            "query_plan_execution_time")
            add_measurement(run_result["metadatas"][0], iteration,
                            "query_planning_time")
            execute("iterteardown")

        if self.perf:
            self.perf.stop()

        # TODO value outlier detection and warning across iterations
        execute("teardown")
        runner.stop()
        self.append_scenario_summary(scenario_name, measurement_sums,
                num_iterations)
        return measurements

    def append_scenario_summary(self, scenario_name, measurement_sums,
            num_iterations):
        self.summary += "{:>30}".format(scenario_name)
        for key in ("query_parsing_time", "query_planning_time",
                "query_plan_execution_time", WALL_TIME):
            if key not in measurement_sums:
                time = "-"
            else:
                time = "{:.10f}".format(measurement_sums[key] / num_iterations)
            self.summary += "{:>30}".format(time)
        self.summary += "\n"


    def runners(self):
        """ Which runners can execute a QuerySuite scenario """
        return ["MemgraphRunner"]

    def groups(self):
        """ Which groups can be executed by a QuerySuite scenario """
        return ["create", "match", "expression", "aggregation", "return",
                "update", "delete", "hardcoded"]


class MemgraphRunner:
    """
    Knows how to start and stop Memgraph (backend) some client frontent
    (bolt), and execute a cypher query.
    Execution returns benchmarking data (execution times, memory
    usage etc).
    """
    def __init__(self, args):
        """
        Creates and configures MemgraphRunner.
        Args:
            args: args to pass to ArgumentParser
        """
        log.info("Initializing MemgraphRunner with arguments %r", args)

        # parse arguments
        argp = ArgumentParser("MemgraphRunnerArgumentParser")
        argp.add_argument("--MemgraphRunnerBin",
                default=os.path.join(os.path.dirname(__file__),
                    "../../../build/memgraph"))
        argp.add_argument("--MemgraphRunnerConfig", required=False)
        argp.add_argument("--MemgraphRunnerURI", default="bolt://localhost:7687")
        argp.add_argument("--MemgraphRunnerEncryptBolt", action="store_true")
        self.args, _ = argp.parse_known_args(args)

        self.memgraph_bin = jail.get_process()
        self.bolt_client = jail.get_process()

    def start(self):
        log.info("MemgraphRunner.start")
        environment = os.environ.copy()
        if self.args.MemgraphRunnerConfig:
            environment["MEMGRAPH_CONFIG"] = self.args.MemgraphRunnerConfig
        self.memgraph_bin.run(self.args.MemgraphRunnerBin, env=environment)
        # TODO change to a check via SIGUSR
        time.sleep(1.0)
        return self.memgraph_bin.get_pid()

    def execute(self, queries):
        log.debug("MemgraphRunner.execute('%s')", str(queries))
        client_args = [path.join(path.dirname(__file__), "bolt_client.py")]
        client_args.append(self.args.MemgraphRunnerURI)
        if (self.args.MemgraphRunnerEncryptBolt):
            client_args.append("--encrypt")
        queries_fd, queries_path = tempfile.mkstemp()
        try:
            queries_file = os.fdopen(queries_fd, "w")
            queries_file.write("\n".join(queries))
            queries_file.close()
        except:
            queries_file.close()
            os.remove(queries_path)
            raise Exception("Writing queries to temporary file failed")

        # TODO make the timeout configurable per query or something
        return_code = self.bolt_client.run_and_wait(
            "python3", client_args, timeout=120, stdin=queries_path)
        os.remove(queries_path)
        if return_code != 0:
            with open(self.bolt_client.get_stderr()) as f:
                stderr = f.read()
            log.error("MemgraphRunner - error while executing queries '%s'. "
                      "Failed with return_code %d and stderr:\n%s",
                      str(queries), return_code, stderr)
            raise Exception("BoltClient execution failed")
        with open(self.bolt_client.get_stdout()) as f:
            return json.loads(f.read())

    def stop(self):
        log.info("MemgraphRunner.stop")
        self.bolt_client.send_signal(jail.SIGKILL)
        self.bolt_client.wait()
        self.memgraph_bin.send_signal(jail.SIGKILL)
        self.memgraph_bin.wait()


def send_data(storage_url, data_type, payload):
    log.info("Sending %d elements of type '%s' to storage at '%s'",
             len(payload), data_type, storage_url)
    log.debug("Sending payload:\n%s", json.dumps(payload, indent=2))
    r = requests.post("%s/store/%s" % (storage_url, data_type), json=payload)
    if r.status_code != 200:
        raise Exception("Unable to send %s data." % data_type)
    log.debug("Storage server response:\n%s", r.json())
    if len(r.json()) == 0:
        raise Exception("Invalid storage server response")
    return r.json()


def send_data_one(storage_url, data_type, payload):
    return send_data(storage_url, data_type, [payload])[0]


def parse_known_args():
    argp = ArgumentParser(description=__doc__)
    # positional, mandatory args
    argp.add_argument("suite", help="Suite to run.")
    argp.add_argument("runner", help="Engine to use.")
    # named, optional arguments
    argp.add_argument("--storage-url", help="URL of the storage server")
    argp.add_argument("--groups", nargs="+", help="Groups to run. If none are"
                      " provided, all available grups are run.")
    argp.add_argument("--scenarios", nargs="+", help="Scenarios to run. If "
                      "none are provided, all available are run.")
    argp.add_argument("--logging", default="INFO", choices=["INFO", "DEBUG"],
                      help="Logging level")
    argp.add_argument("--additional-run-fields", default={}, type=json.loads,
                      help="Additional fields to add to the 'run', in JSON")
    return argp.parse_known_args()


def main():
    args, remaining_args = parse_known_args()
    if args.logging:
        logging.basicConfig(level=args.logging)
        logging.getLogger("requests").setLevel(logging.WARNING)
        logging.getLogger("urllib3").setLevel(logging.WARNING)
        logging.getLogger("neo4j.bolt").setLevel(logging.WARNING)
    log.info("Memgraph benchmark suite harness")
    log.info("Executing for suite '%s', runner '%s', storage '%s'",
             args.suite, args.runner, args.storage_url)

    # Create suite
    suites = {"QuerySuite": QuerySuite}
    if args.suite not in suites:
        raise Exception(
            "Suite '{}' isn't registered. Registered suites are: {}".format(
                args.suite, suites))
    suite = suites[args.suite](remaining_args)

    # Load scenarios
    group_scenarios = suites[args.suite].scenarios(remaining_args)
    log.info("Loaded %d groups, with a total of %d scenarios",
             len(group_scenarios),
             sum([len(x) for x in group_scenarios.values()]))

    # Create runner
    runners = {"MemgraphRunner": MemgraphRunner}
    # TODO if make runner argument optional, then execute all runners
    if args.runner not in suite.runners():
        raise Exception("Runner '{}' not registered for suite '{}'".format(
            args.runner, args.suite))
    runner = runners[args.runner](remaining_args)

    # Validate groups (if provided)
    if args.groups:
        for group in args.groups:
            if group not in suite.groups():
                raise Exception("Group '{}' isn't registered for suite '{}'".
                                format(group, suite))
        groups = args.groups
    else:
        # No groups provided, use all suite group
        groups = suite.groups()

    # TODO enable scenario filtering on regex
    filtered_scenarios = OrderedDict()
    for group, scenarios in group_scenarios.items():
        if group not in groups:
            log.info("Skipping group '%s'", group)
            continue
        for scenario_name, scenario in scenarios:
            if args.scenarios and scenario_name not in args.scenarios:
                continue
            filtered_scenarios[(group, scenario_name)] = scenario

    if (len(filtered_scenarios) == 0):
        log.info("No scenarios to execute")
        return

    log.info("Executing %d scenarios", len(filtered_scenarios))
    results = []
    for (group, scenario_name), scenario in filtered_scenarios.items():
        log.info("Executing group.scenario '%s.%s' with elements %s",
                 group, scenario_name, list(scenario.keys()))
        for iter_result in suite.run(scenario, scenario_name, runner):
            iter_result['group'] = group
            iter_result['scenario'] = scenario_name
            results.append(iter_result)
    run = dict()
    run["suite"] = args.suite
    run["runner"] = runner.__class__.__name__
    run["runner_config"] = vars(runner.args)
    run.update(args.additional_run_fields)
    if args.storage_url is not None:
        run_uuid = send_data_one(args.storage_url, "run", run)
        for result in results:
            result["run"] = run_uuid
        send_data(args.storage_url, "measurement", results)
    print("\n\n{}\n".format(suite.summary))


if __name__ == "__main__":
    main()
