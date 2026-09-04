"""Drive the flasher page against a fake Web Serial mule.

The fake speaks exactly what mule/main/serial_config.c speaks: newline JSON in,
one "HMSMM <json>" line out, with ESP_LOG-shaped noise interleaved so the prefix
scan is actually exercised rather than assumed. Nothing here needs hardware, and
it catches the failures that hardware would only show you slowly: a reply
consumed by a stale waiter, credentials sent to the miner, a port left locked
after a failed attempt.

Run it against a staged site, the way pages.yml assembles one:

    mkdir -p /tmp/mmsite/firmware
    cp mule/build/merged.bin  /tmp/mmsite/firmware/mule-1.0.1-merged.bin
    cp miner/build/merged.bin /tmp/mmsite/firmware/miner-1.0.0-merged.bin
    python3 .github/scripts/build_flasher_manifests.py /tmp/mmsite/firmware
    sed -e 's/__MULE_VERSION__/1.0.1/g' -e 's/__MINER_VERSION__/1.0.0/g' \
        -e 's/__TAG__/v1.0.1/g' docs/flasher/index.html > /tmp/mmsite/index.html
    SITE=/tmp/mmsite python3 .github/scripts/test_flasher.py

Needs `pip install playwright` and a chromium it can launch. If playwright's own
download is missing or the wrong build, point CHROME at one you have.
"""
import http.server, json, os, socketserver, threading, sys
from playwright.sync_api import sync_playwright

SITE = os.environ.get("SITE") or os.path.join(os.path.dirname(os.path.abspath(__file__)), "site")

class Handler(http.server.SimpleHTTPRequestHandler):
    def __init__(self, *a, **kw):
        super().__init__(*a, directory=SITE, **kw)
    def log_message(self, *a):
        pass

httpd = socketserver.TCPServer(("127.0.0.1", 0), Handler)
port = httpd.server_address[1]
threading.Thread(target=httpd.serve_forever, daemon=True).start()
URL = f"http://localhost:{port}/"

# role / behaviour of the fake board is switched per test via window.__fake
FAKE_SERIAL = """
window.__fake = { role: 'mule', answer: true, silentBefore: 0 };
window.__sent = [];
class FakePort {
  constructor() {
    this.controller = null;
    this.readable = new ReadableStream({ start: (c) => { this.controller = c; } });
    const enc = new TextEncoder();
    const self = this;
    this.writable = new WritableStream({
      write(chunk) {
        const text = new TextDecoder().decode(chunk);
        for (const line of text.split('\\n')) {
          if (!line.trim()) continue;
          const cmd = JSON.parse(line);
          window.__sent.push(cmd);
          // ESP_LOG noise on the same port, before and after the reply.
          self.emit('I (1234) MAIN: WiFi: DOWN');
          if (window.__fake.silentBefore > 0) { window.__fake.silentBefore--; continue; }
          if (!window.__fake.answer) continue;
          if (cmd.cmd === 'ping') {
            self.emit('HMSMM ' + JSON.stringify({ ok: true, role: window.__fake.role,
              fw: "1.0.1", serial: 'MM-1A2B', wifi: false, ezshare: false }));
          } else if (cmd.cmd === 'provision') {
            if (!cmd.ssid) self.emit('HMSMM ' + JSON.stringify({ ok: false, error: 'ssid is required' }));
            else self.emit('HMSMM ' + JSON.stringify({ ok: true, restarting: true }));
          }
          self.emit('I (1250) SERCFG: USB provisioning ready');
        }
      }
    });
    this.emit = (line) => this.controller.enqueue(enc.encode(line + '\\r\\n'));
  }
  async open() {}
  async close() { try { this.controller.close(); } catch (e) {} }
}
Object.defineProperty(navigator, 'serial', {
  configurable: true,
  value: { requestPort: async () => new FakePort() },
});
"""

failures = []

def check(name, condition, detail=""):
    print(("PASS " if condition else "FAIL ") + name + (f"  {detail}" if detail and not condition else ""))
    if not condition:
        failures.append(name)

with sync_playwright() as p:
    chrome = os.environ.get("CHROME")
    browser = p.chromium.launch(executable_path=chrome) if chrome else p.chromium.launch()
    ctx = browser.new_context()
    ctx.add_init_script(FAKE_SERIAL)
    page = ctx.new_page()
    errors = []
    page.on("console", lambda m: errors.append(m.text) if m.type == "error" else None)
    page.on("pageerror", lambda e: errors.append(str(e)))

    page.goto(URL, wait_until="networkidle")

    # --- static page ---
    check("mule version substituted", "1.0.1" in page.inner_text(".sub"))
    check("miner version substituted", "1.0.0" in page.inner_text(".sub"))
    check("no placeholders left", "__" not in page.inner_text("body"))
    check("install button upgraded",
          page.evaluate("!!customElements.get('esp-web-install-button')"))
    check("default manifest is mule",
          page.get_attribute("#installer", "manifest") == "firmware/manifest-mule.json")

    page.click(".board:has(input[value=miner])")
    check("picker swaps manifest",
          page.get_attribute("#installer", "manifest") == "firmware/manifest-miner.json")
    page.click(".board:has(input[value=mule])")

    # manifests resolve the way the browser will fetch them
    for board in ("mule", "miner"):
        r = page.request.get(f"{URL}firmware/manifest-{board}.json")
        m = r.json()
        asset = page.request.get(f"{URL}firmware/{m['builds'][0]['parts'][0]['path']}")
        check(f"{board} manifest firmware fetches", asset.ok and len(asset.body()) > 500_000)
        check(f"{board} chipFamily is C3", m["builds"][0]["chipFamily"] == "ESP32-C3")

    # --- empty SSID is refused before any port is opened ---
    page.fill("#ssid", "")
    page.click("#send")
    page.wait_for_timeout(200)
    # `required` means the browser refuses the submit itself; the JS guard behind
    # it only matters for whitespace-only input, checked next.
    check("empty ssid blocked by the browser",
          not page.evaluate("document.getElementById('ssid').checkValidity()"))
    check("no port requested for empty ssid", page.evaluate("window.__sent.length") == 0)

    page.fill("#ssid", "   ")
    page.click("#send")
    page.wait_for_timeout(300)
    check("whitespace ssid blocked by the page",
          "network name" in page.inner_text("#status").lower(),
          page.inner_text("#status"))
    check("no port requested for whitespace ssid", page.evaluate("window.__sent.length") == 0)

    # --- happy path ---
    page.fill("#ssid", "scorpio")
    page.fill("#pass", "hunter2")
    page.fill("#ez_ssid", "ez Share")
    page.fill("#ez_pass", "88888888")
    page.click("#send")
    page.wait_for_selector("#status.ok", timeout=15000)
    check("happy path reports saved", "Saved" in page.inner_text("#status"))
    sent = page.evaluate("window.__sent")
    prov = [c for c in sent if c["cmd"] == "provision"]
    check("provision sent once", len(prov) == 1, json.dumps(sent))
    check("credentials carried intact",
          prov[0] == {"cmd": "provision", "ssid": "scorpio", "pass": "hunter2",
                      "ez_ssid": "ez Share", "ez_pass": "88888888"}, json.dumps(prov))
    check("button re-enabled after success", not page.is_disabled("#send"))

    # --- ping retried when the first attempts get no answer ---
    page.evaluate("window.__sent = []; window.__fake.silentBefore = 2;")
    page.click("#send")
    page.wait_for_selector("#status.ok", timeout=20000)
    pings = [c for c in page.evaluate("window.__sent") if c["cmd"] == "ping"]
    check("ping retried past silence", len(pings) >= 3, f"pings={len(pings)}")

    # --- wrong board ---
    page.evaluate("window.__sent = []; window.__fake.role = 'miner';")
    page.click("#send")
    page.wait_for_selector("#status.bad", timeout=15000)
    check("miner rejected", "miner" in page.inner_text("#status"))
    check("no credentials sent to the miner",
          all(c["cmd"] != "provision" for c in page.evaluate("window.__sent")))

    # --- nothing answers at all ---
    page.evaluate("window.__sent = []; window.__fake.role = 'mule'; window.__fake.answer = false;")
    page.click("#send")
    page.wait_for_selector("#status.bad", timeout=30000)
    check("silent board reported", "Nothing answered" in page.inner_text("#status"))
    check("button re-enabled after failure", not page.is_disabled("#send"))

    # --- retry works after a failure (port was not left locked) ---
    page.evaluate("window.__sent = []; window.__fake.answer = true;")
    page.click("#send")
    page.wait_for_selector("#status.ok", timeout=15000)
    check("retry after failure succeeds", "Saved" in page.inner_text("#status"))

    if os.environ.get("SCREENSHOT"):
        page.screenshot(path=os.environ["SCREENSHOT"], full_page=True)
    check("no console errors", not errors, "; ".join(errors[:5]))
    browser.close()

print()
print(f"{len(failures)} failure(s)" + (": " + ", ".join(failures) if failures else ""))
sys.exit(1 if failures else 0)
