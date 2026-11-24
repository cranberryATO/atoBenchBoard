"""GPIO configuration readout."""


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
