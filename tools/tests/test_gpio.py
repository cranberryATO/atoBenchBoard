"""GPIO configuration readout and outputs."""

import pytest

LED = "PA5"  # LD2 of the Nucleo board, GPIO_Output in CubeMX


def pins_by_name(cli) -> dict:
    """{"PA4": {...}, ...} for the ports with their clock enabled."""
    pins = {}
    for port in cli.command("gpio show")["ports"]:
        for pin in port["pins"]:
            pins[f"P{port['port'][-1]}{pin['pin']}"] = pin
    return pins


def test_gpio_show_ports(cli):
    ports = cli.command("gpio show")["ports"]
    assert [p["port"] for p in ports] == [f"GPIO{c}" for c in "ABCDEFG"]
    for p in ports:
        if not p["clock"]:
            assert p["pins"] == []


def test_gpio_show_scope_pins_are_analog(cli):
    pins = pins_by_name(cli)
    for name in ("PA4", "PA0", "PB14"):  # DAC1_OUT1, ADC1_IN1, ADC1_IN5
        assert pins[name]["mode"] == "ANL", name


def test_gpio_show_cli_uart_pins(cli):
    pins = pins_by_name(cli)
    for name in ("PC4", "PC5"):  # USART1 TX / RX
        assert pins[name]["mode"] == "AF", name
        assert "USART1" in pins[name]["af"], name


@pytest.mark.parametrize("line, error", [
    ("gpio set PA0 1", "PA0 is not an output"),
    ("gpio set PE3 1", "no pin 'PE3' in this package"),
    ("gpio set PH0 1", "no pin 'PH0' in this package"),
    ("gpio set PA16 1", "no pin 'PA16' in this package"),
    ("gpio set XA1 1", "no pin 'XA1' in this package"),
    ("gpio set PA5 2", "<level>: out of range 0..1"),
])
def test_gpio_set_errors(cli, line, error):
    assert cli.request(line)["error"] == error


def test_gpio_set_output(cli):
    if pins_by_name(cli)[LED]["mode"] != "OUT":
        pytest.skip(f"{LED} is not configured as an output in CubeMX")
    try:
        for level in (1, 0):
            assert cli.command(f"gpio set {LED} {level}") == {
                "pin": LED, "level": level, "ok": True}
            assert pins_by_name(cli)[LED]["level"] == level
        # the pin name is not case sensitive
        assert cli.command(f"gpio set {LED.lower()} 0")["pin"] == LED
    finally:
        cli.request(f"gpio set {LED} 0")
