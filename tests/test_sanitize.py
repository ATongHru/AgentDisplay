from sanitize import mask_wifi_profile, sanitize_ble_line


def test_sanitize_wifi_command():
    assert sanitize_ble_line("WIFI:MyNet,SuperSecret") == "WIFI:MyNet,********"


def test_sanitize_token_command():
    assert sanitize_ble_line("TOKEN:abc123") == "TOKEN:********"


def test_sanitize_ble_pass_field():
    assert sanitize_ble_line("OK P i=0 ssid=foo pass=abc123 host=1.2.3.4:8000") == (
        "OK P i=0 ssid=foo pass=******** host=1.2.3.4:8000"
    )


def test_mask_wifi_profile():
    masked = mask_wifi_profile({"ssid": "a", "password": "pw"})
    assert masked["password"] == "********"
    revealed = mask_wifi_profile({"ssid": "a", "password": "pw"}, reveal_password=True)
    assert revealed["password"] == "pw"
