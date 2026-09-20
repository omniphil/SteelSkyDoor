#!/usr/bin/env python3
"""A fake TERMinator on a pty, so the door can be tested without Windows or a BBS.

This is the protocol in executable form. It answers Query, plays the part of the
asset cache, checks that the door names the game files correctly afterwards, and
runs a saved game out and back again.

    python3 test_door.py              assets already cached (a returning player)
    python3 test_door.py --upload     make the door actually send the 72 MB
    python3 test_door.py --bin        binary frames instead of base64
    python3 test_door.py --nomouse    an older TERMinator with no mouse

By default the assets are answered with "Have", because a real upload moves
72 MB through a pty and takes minutes. --upload does the slow, thorough version
and re-hashes everything the door sends.
"""
import base64
import hashlib
import os
import pty
import re
import select
import shutil
import sys
import time

MODULE = "steelsky"
APC = "\033_TERMinator:TRACE;"
ST = "\033\\"
ESCAPED = {0x00, 0x0A, 0x0D, 0x11, 0x13, 0x18, 0x1B, ord('='), 0xFF}

fails = []


def check(ok, what):
    print(("  ok   " if ok else "  FAIL ") + what)
    if not ok:
        fails.append(what)


def unescape(d):
    out, i = bytearray(), 0
    while i < len(d):
        if d[i] == ord('=') and i + 1 < len(d):
            out.append((d[i + 1] - 64) & 0xFF)
            i += 2
        else:
            out.append(d[i])
            i += 1
    return bytes(out)


def escape(d):
    """Module -> door. TERMinator escapes EVERY control byte on this path, not
    just the nine the door escapes going the other way: a door reads through a
    pty, which would otherwise mangle CR, XON/XOFF and the rest. Escaping only
    the door's set looks fine until a save happens to contain a 0x0D."""
    out = bytearray()
    for b in d:
        if b < 0x20 or b == 0x7F or b == ord('=') or b == 0xFF:
            out += bytes((ord('='), (b + 64) & 0xFF))
        else:
            out.append(b)
    return bytes(out)


class Fake:
    def __init__(self, binary, mouse, upload):
        self.binary, self.mouse, self.upload = binary, mouse, upload
        self.buf = b""
        self.raw = b""
        self.offered = []          # (sha, size) the door offered, in order
        self.uploaded = {}         # sha -> bytes, when --upload
        self.pending = {}
        self.module_sha = None
        self.files_msg = None      # the "files sky.dsk=..." line
        self.saves = {}            # name -> bytes, what the door sent us
        self.open_opts = ""

    # -- transport ---------------------------------------------------------
    def feed(self, chunk):
        self.buf += chunk
        self.raw = (self.raw + chunk)[-8192:]
        cmds = []
        while True:
            s = self.buf.find(b"\033_")
            if s < 0:
                self.buf = self.buf[-2:]
                break
            e = self.buf.find(b"\033\\", s)
            if e < 0:
                break
            cmds.append(self.buf[s + 2:e])
            self.buf = self.buf[e + 2:]
        return cmds

    def send(self, fd, text):
        os.write(fd, (APC + text + ST).encode("latin-1"))

    def send_module_data(self, fd, payload):
        """Module -> door. TERMinator escapes every control byte on this path."""
        if self.binary:
            head = f"Data;module={MODULE}".encode()
            os.write(fd, b"\033_TERMinator:TRACE;Bin;" +
                     escape(head + b"\n" + payload) + b"\033\\")
        else:
            self.send(fd, f"Data;module={MODULE};b64=" +
                      base64.b64encode(payload).decode())

    # -- commands ----------------------------------------------------------
    def handle(self, fd, cmd):
        text = cmd.decode("latin-1", "replace")
        if not text.startswith("TERMinator:TRACE;"):
            return
        body = text[len("TERMinator:TRACE;"):]

        if body.startswith("Query"):
            caps = ("Info;v=2;wasm=1;audio=1;assets=1;send=1;store=1;tick=1"
                    + (";text=1;mouse=1;pad=1;import=1" if self.mouse else "")
                    + (";bin=1" if self.binary else ""))
            self.send(fd, caps)
            return

        if body.startswith("Asset;"):
            sha = re.search(r"sha256=([0-9a-f]{64})", body).group(1)
            size = int(re.search(r"size=(\d+)", body).group(1))
            self.offered.append((sha, size))
            if self.upload:
                self.pending[sha] = bytearray()
                self.send(fd, f"NeedAsset;module={MODULE}")
            else:
                self.send(fd, f"Have;module={MODULE};sha256={sha}")
            return

        if body.startswith("Open;"):
            self.open_opts = body
            sha = re.search(r"wasm=([0-9a-f]{64})", body).group(1)
            self.module_sha = sha
            self.send(fd, f"Ready;module={MODULE}")
            return

        if body.startswith("Bin;"):
            payload = unescape(cmd[len("TERMinator:TRACE;Bin;"):])
            nl = payload.find(b"\n")
            head = (payload[:nl] if nl >= 0 else payload).decode("latin-1", "replace")
            if head.startswith("Put;"):
                self.take_put(head, payload[nl + 1:])
            elif head.startswith("Data;"):
                self.take_data(payload[nl + 1:])
            return

        if body.startswith("Put;"):
            head, _, b64 = body.partition(";data=")
            self.take_put(head, base64.b64decode(b64))
            return

        if body.startswith("PutDone;"):
            m = re.search(r"asset=([0-9a-f]{64})", body)
            if m:
                sha = m.group(1)
                got = bytes(self.pending.pop(sha, b""))
                check(hashlib.sha256(got).hexdigest() == sha,
                      f"asset hash matches ({len(got):,} bytes)")
                self.uploaded[sha] = got
                self.send(fd, f"Have;module={MODULE};sha256={sha}")
            return

        if body.startswith("Data;"):
            prefix = f"Data;module={MODULE};"
            if ";b64=" in body:
                self.take_data(base64.b64decode(body.split(";b64=", 1)[1]))
            elif body.startswith(prefix):
                self.take_text(body[len(prefix):])
            return

        if body.startswith("Close;"):
            self.send(fd, f"Closed;module={MODULE}")

    def take_put(self, head, data):
        off = int(re.search(r"offset=(\d+)", head).group(1))
        m = re.search(r"asset=([0-9a-f]{64})", head)
        key = m.group(1) if m else "module"
        buf = self.pending.setdefault(key, bytearray())
        if len(buf) < off + len(data):
            buf.extend(b"\0" * (off + len(data) - len(buf)))
        buf[off:off + len(data)] = data

    def take_data(self, payload):
        """head\\npayload, as tdoor_send() builds it."""
        nl = payload.find(b"\n")
        if nl < 0:
            self.take_text(payload.decode("latin-1", "replace"))
            return
        head = payload[:nl].decode("latin-1", "replace")
        body = payload[nl + 1:]
        m = re.match(r"save (\S+) (\d+) (\d+)", head)
        if m:
            name, off, total = m.group(1), int(m.group(2)), int(m.group(3))
            cur = self.saves.setdefault(name, bytearray(total))
            if len(cur) < total:
                cur.extend(b"\0" * (total - len(cur)))
            cur[off:off + len(body)] = body

    def take_text(self, text):
        if text.startswith("files "):
            self.files_msg = text


def main():
    binary = "--bin" in sys.argv
    mouse = "--nomouse" not in sys.argv
    upload = "--upload" in sys.argv
    print("Steel Sky door test (%s, %s, %s)" % (
        "binary frames" if binary else "base64",
        "mouse" if mouse else "no mouse",
        "real upload" if upload else "assets cached"))

    here = os.path.dirname(os.path.abspath(__file__))
    for need in ("steelskydoor", "steelsky.wasm", "sky.dsk", "sky.cpt", "sky.dnr"):
        if not os.path.exists(os.path.join(here, need)):
            print(f"missing {need} -- run make && make install")
            return 1

    # Start from a known save, so the door has something to send us.
    folder = os.path.join(here, "saves", "Player-0")
    shutil.rmtree(os.path.join(here, "saves"), ignore_errors=True)
    os.makedirs(folder, exist_ok=True)
    known = bytes((i * 37 + 11) & 0xFF for i in range(5000))
    with open(os.path.join(folder, "SKY-VM.001"), "wb") as f:
        f.write(known)

    pid, fd = pty.fork()
    if pid == 0:
        os.chdir(here)
        os.execv("./steelskydoor", ["./steelskydoor"])
        os._exit(1)

    fake = Fake(binary, mouse, upload)
    deadline = time.time() + (900 if upload else 180)
    sent_put = False
    put_at = 0
    new_save = bytes((i * 91 + 7) & 0xFF for i in range(4200))

    try:
        while time.time() < deadline:
            r, _, _ = select.select([fd], [], [], 0.2)
            if r:
                try:
                    chunk = os.read(fd, 65536)
                except OSError:
                    break
                if not chunk:
                    break
                for cmd in fake.feed(chunk):
                    fake.handle(fd, cmd)

            # Once the door has named the files and handed over the save, act
            # like the module writing a new one.
            if fake.files_msg and fake.saves and not sent_put:
                sent_put = True
                put_at = time.time()
                for off in range(0, len(new_save), 3000):
                    part = new_save[off:off + 3000]
                    head = f"put SKY-VM.002 {off} {len(new_save)}\n".encode()
                    fake.send_module_data(fd, head + part)
                print("  --   module wrote a save (SKY-VM.002)")

            if sent_put and time.time() - put_at > 3:
                break

        # ---- assertions
        check(fake.module_sha is not None, "the door offered the module")
        check("exclusive=1" in fake.open_opts, "Open asks for the whole screen")
        check(len(fake.offered) >= 4,
              f"the door offered {len(fake.offered)} assets (sky.dsk split + cpt + dnr)")
        check(all(sz <= 64 * 1024 * 1024 for _, sz in fake.offered),
              "every asset fits TERMinator's 64 MB cap")

        # the "files" line must name exactly the assets that were offered
        check(fake.files_msg is not None, "the door named the game files")
        if fake.files_msg:
            named = re.findall(r"[0-9a-f]{64}", fake.files_msg)
            check(named == [sha for sha, _ in fake.offered],
                  "and named exactly the assets it offered, in order")
            check("sky.cpt=" in fake.files_msg and "sky.dnr=" in fake.files_msg,
                  "including sky.cpt and sky.dnr")
            dsk = re.search(r"sky\.dsk=([0-9a-f,]+)", fake.files_msg)
            check(dsk and "," in dsk.group(1),
                  "sky.dsk is named as several assets, as it must be")

        got = bytes(fake.saves.get("SKY-VM.001", b""))
        check(got == known, f"the player's existing save was sent out intact ({len(got):,} bytes)")

        time.sleep(0.5)
        back = os.path.join(folder, "SKY-VM.002")
        check(os.path.exists(back), "the save the module wrote reached the BBS")
        if os.path.exists(back):
            check(open(back, "rb").read() == new_save, "and is byte-identical")
        check(not os.path.exists(back + ".part"), "no half-written .part left behind")

    finally:
        try:
            os.write(fd, b"\033")
            time.sleep(0.3)
            os.close(fd)
        except OSError:
            pass
        try:
            os.kill(pid, 9)
            os.waitpid(pid, 0)
        except (OSError, ChildProcessError):
            pass

    print()
    if fails:
        print(f"FAILED ({len(fails)}): " + "; ".join(fails))
        return 1
    print("all checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
