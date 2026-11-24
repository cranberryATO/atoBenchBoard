"""Interpreter: modes, help, parameter checks, menus, line editor."""

import pytest

EXPECTED_COMMANDS = {
    "help", "mode", "ps", "so", "stats", "adc", "dac", "gen sine",
    "gen square", "gen step", "acq", "dump", "gpio show", "gpio set", "pwm", "adc list", "dac list",
}


def test_mode_json(cli):
    assert cli.command("mode json") == {"mode": "json", "ok": True}


def test_help_lists_the_commands(cli):
    commands = cli.command("help")["commands"]
    names = {c["command"] for c in commands}
    assert EXPECTED_COMMANDS <= names
    for c in commands:
        assert c["usage"].startswith(c["command"])
        assert c["help"]


def test_help_filters_on_a_word(cli):
    names = {c["command"] for c in cli.command("help gen")["commands"]}
    assert names == {"gen sine", "gen square", "gen step"}


def test_help_unknown_word(cli):
    reply = cli.request("help nothing")
    assert reply["ok"] is False
    assert "no command" in reply["error"]


def test_unknown_command(cli):
    reply = cli.request("nothing")
    assert reply["ok"] is False
    assert "unknown command" in reply["error"]


@pytest.mark.parametrize("line, error", [
    ("acq", "usage: acq <fs> <n>"),
    ("acq 10000", "usage: acq <fs> <n>"),
    ("acq 10000 100 1", "usage: acq <fs> <n>"),
    ("acq 5 100", "<fs>: out of range 10..200000"),
    ("acq 10000 5000", "<n>: out of range 16..4096"),
    ("acq 10x 100", "<fs>: invalid number '10x'"),
    ("mode xml", "<mode>: expected plain|json"),
])
def test_parameter_errors(cli, line, error):
    reply = cli.request(line)
    assert reply == {"error": error, "ok": False}


@pytest.mark.parametrize("line", ["gen step 20k", "gen step 0x10000"])
def test_number_formats_are_parsed(cli, line):
    # k suffix and hexadecimal are numbers: the error is the range
    assert "out of range" in cli.request(line)["error"]


def test_number_formats_in_range(cli):
    assert cli.command("gen step 0x3e8")["vpp"] == 1000
    assert cli.command("gen sine 1k 1000")["f"] == 1000


def test_too_many_words(cli):
    reply = cli.request(" ".join(["w"] * 11))
    assert reply == {"error": "too many words", "ok": False}


def test_line_too_long(cli):
    reply = cli.request("x" * 150)
    assert reply["ok"] is False
    assert "too long" in reply["error"]
    # the interpreter is still in sync: the next line gets its own reply
    assert cli.command("mode json")["mode"] == "json"


def test_menu_enter_use_and_leave(cli):
    assert cli.command("gen")["menu"] == "gen"
    # in the menu, the name relative to the menu
    assert cli.command("sine 100 1000")["shape"] == "sine"
    # root commands and full names work from a menu
    assert cli.command("stats")["ok"]
    assert cli.command("gen step 1000")["shape"] == "step"
    assert cli.command("..")["menu"] == ""


def test_menu_root(cli):
    cli.command("gen")
    assert cli.command("/")["menu"] == ""
    # back at the root, a name relative to "gen" is unknown
    assert cli.request("sine 100 1000")["ok"] is False
