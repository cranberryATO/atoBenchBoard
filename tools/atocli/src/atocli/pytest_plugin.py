"""pytest plugin of the test harness: options and fixtures.

Loaded through the "pytest11" entry point of atocli, i.e. before the command
line is parsed: a conftest.py option would be unknown at that time, and its
value (an existing path such as /dev/ttyUSB0) taken as a test path.

Usage, with the board connected and running:
    uv run pytest --port /dev/ttyUSB0
or with the port in the ATO_CLI_PORT environment variable. Tests needing a
wiring are skipped unless enabled (--loopback).
"""

import os

import pytest

from atocli import Cli


def pytest_addoption(parser):
    group = parser.getgroup("atocli", "atoBenchBoard CLI")
    group.addoption("--port", default=os.environ.get("ATO_CLI_PORT"),
                    help="serial port of the CLI (USART1), "
                         "default: $ATO_CLI_PORT")
    group.addoption("--baud", type=int, default=115200)
    group.addoption("--loopback", action="store_true",
                    help="run the tests marked loopback (scope wiring)")


def pytest_configure(config):
    config.addinivalue_line(
        "markers",
        "loopback: needs the scope wiring, PA4 (DAC1_OUT1) to PA0 (ADC1_IN1) "
        "and PB14 (ADC1_IN5); run with --loopback")


def pytest_collection_modifyitems(config, items):
    if config.getoption("--loopback"):
        return
    skip = pytest.mark.skip(reason="needs the scope wiring, use --loopback")
    for item in items:
        if "loopback" in item.keywords:
            item.add_marker(skip)


@pytest.fixture(scope="session")
def cli_session(request):
    """One connection for the whole session, in json mode."""
    port = request.config.getoption("--port")
    if not port:
        pytest.exit("no CLI port: use --port /dev/ttyUSB0 or ATO_CLI_PORT",
                    returncode=2)
    try:
        cli = Cli(port, request.config.getoption("--baud"))
    except (OSError, TimeoutError) as e:  # SerialException is an OSError
        pytest.exit(f"cannot use the CLI on {port}: {e}", returncode=2)
    with cli:
        yield cli


@pytest.fixture
def cli(cli_session):
    """The CLI, back in the root menu: each test starts from the same state."""
    cli_session.command("/")
    return cli_session
