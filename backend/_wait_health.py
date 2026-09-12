import sys
import time
import urllib.request

url = "http://127.0.0.1:8000/health"
once = "--once" in sys.argv
tries = 1 if once else 40
delay = 0.25
for _ in range(tries):
    try:
        urllib.request.urlopen(url, timeout=1)
        raise SystemExit(0)
    except SystemExit:
        raise
    except Exception:
        if once:
            raise SystemExit(1)
        time.sleep(delay)
raise SystemExit(1)
