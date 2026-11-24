"""System commands: tasks, UART counters."""

TASK_STATES = set("XRBSD?")


def test_ps_lists_the_tasks(cli):
    tasks = cli.command("ps")["tasks"]
    names = {t["name"] for t in tasks}
    assert {"CLI1", "IDLE"} <= names
    for t in tasks:
        assert t["state"] in TASK_STATES
        assert isinstance(t["prio"], int)
        assert t["stack"] > 0, f"{t['name']}: stack exhausted"


def test_ps_the_cli_task_is_running(cli):
    tasks = {t["name"]: t for t in cli.command("ps")["tasks"]}
    assert tasks["CLI1"]["state"] == "X"  # it is running the command


def test_stats_counters(cli):
    reply = cli.command("stats")
    for key in ("tx_dropped_bytes", "rx_errors", "tx_errors",
                "rx_restart_failures"):
        assert isinstance(reply[key], int)
    # writes of the CLI are blocking: nothing is ever dropped
    assert reply["tx_dropped_bytes"] == 0
