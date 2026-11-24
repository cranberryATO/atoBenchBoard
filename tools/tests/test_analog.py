"""Analog commands: DAC, ADC, waveform generator and acquisition."""

import pytest

VREF_MV = 3300
FULL_SCALE = 4095


def test_dac_set(cli):
    reply = cli.command("dac 1 1 1000")
    assert reply["raw"] == 1000 * FULL_SCALE // VREF_MV
    assert abs(reply["mv"] - 1000) <= 1


@pytest.mark.parametrize("line, error", [
    ("dac 2 1 1000", "no DAC2"),
    ("dac 1 1 4000", "<mv>: out of range 0..3300"),
    ("dac 1 3 1000", "<ch>: out of range 1..2"),
])
def test_dac_errors(cli, line, error):
    assert cli.request(line)["error"] == error


def test_adc_of_the_scope_is_not_available(cli):
    assert "timer triggered" in cli.request("adc 1 1")["error"]


def test_adc_missing(cli):
    assert cli.request("adc 3 1")["error"] == "no ADC3"


@pytest.mark.parametrize("shape", ["sine", "square"])
def test_gen_periodic(cli, shape):
    assert cli.command(f"gen {shape} 100 2000") == {
        "shape": shape, "f": 100, "vpp": 2000, "ok": True}


def test_gen_step(cli):
    assert cli.command("gen step 2000")["shape"] == "step"


def test_acq_frequency_does_not_fit(cli):
    cli.command("gen sine 1000 2000")
    # 10 Hz sampling: fewer than 4 points per period
    assert "f must be between" in cli.request("acq 10 100")["error"]


def acquire(cli, fs: int, n: int) -> dict:
    timeout = n / fs + 2  # acquisition + settling delay
    reply = cli.command(f"acq {fs} {n}", timeout=timeout)
    assert reply["n"] == n
    dump = cli.command("dump", reply_bytes=10 * n + 300)
    assert dump["n"] == n
    assert len(dump["in"]) == n and len(dump["out"]) == n
    return reply | dump


def test_acq_and_dump(cli):
    cli.command("gen step 2000")
    d = acquire(cli, 10000, 100)
    assert d["fs"] == pytest.approx(10000, rel=1e-3)
    assert d["shape"] == "step" and d["vpp"] == 2000
    assert d["vref"] == VREF_MV and d["full"] == FULL_SCALE
    assert all(0 <= v <= FULL_SCALE for v in d["in"] + d["out"])


def test_acq_sine_actual_frequency(cli):
    cli.command("gen sine 160 2000")
    d = acquire(cli, 20000, 200)
    # f = fs / round(fs / 160): close to the request, not exact
    assert d["f"] == pytest.approx(160, rel=0.01)


@pytest.mark.loopback
def test_loopback_step_levels(cli):
    """DAC output wired to both ADC inputs: they see the step levels."""
    cli.command("gen step 2000")
    d = acquire(cli, 10000, 800)
    half = 2000 * FULL_SCALE // VREF_MV // 2
    low, high = 2048 - half, 2048 + half
    tol = 60  # raw counts, ~50 mV
    for ch in ("in", "out"):
        v = d[ch]
        # first eighth: low level; well after the step: high level
        assert all(abs(x - low) < tol for x in v[5:90]), ch
        assert all(abs(x - high) < tol for x in v[150:]), ch
