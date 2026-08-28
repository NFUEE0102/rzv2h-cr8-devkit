#!/usr/bin/env python3
"""PWM 網頁示範後端(EVK ~/r8web/serve_pwm.py,要 sudo 跑 —— /dev/uio 需要 root)。

架構:
    瀏覽器 --HTTP--> 本 server --stdin 一行一筆--> r8_bench pwmd(常駐,獨佔 rpmsg)
                                 <--/run/r8pwm.json(daemon 每筆原子回寫)--

為什麼隔一層 daemon:rpmsg 通道必須單一常駐 client 持有(doc 16:
反覆建立/拆除會把 MHU 停在半個握手),HTTP server 卻是多執行緒短連線。
所以 server 只做兩件事:序列化指令寫進 daemon stdin、等 seq 前進後把
R8 回報的 JSON 轉給瀏覽器。

停止:Ctrl-C → 關 daemon stdin(EOF)→ daemon 走完整 release_channel 收攤。
絕不 SIGKILL daemon(MHU 會卡死)。
"""
import http.server
import json
import os
import pathlib
import signal
import socketserver
import subprocess
import sys
import threading
import time

PORT       = int(os.environ.get("PWM_PORT", "8080"))
HERE       = pathlib.Path(__file__).resolve().parent
BENCH_DIR  = pathlib.Path("/home/ubuntu/r8_bench")
JSON_PATH  = pathlib.Path("/run/r8pwm.json")
DAEMON_LOG = pathlib.Path("/run/r8pwmd.log")

VALID_GPT = {0, 4, 5, 6, 7, 8, 9}


class Daemon:
    """持有 r8_bench pwmd 子行程;所有指令經 send() 序列化。"""

    def __init__(self):
        self.proc = None
        self.lock = threading.Lock()
        self.last_restart = 0.0

    def alive(self):
        return self.proc is not None and self.proc.poll() is None

    def start(self):
        # 舊 JSON 是上一個 daemon 的 seq 序列;新 daemon 從 1 重數,
        # 不清掉的話 send() 的「seq 前進」判斷會永遠等不到。
        JSON_PATH.unlink(missing_ok=True)
        log = open(DAEMON_LOG, "ab", buffering=0)
        self.proc = subprocess.Popen(
            ["./r8_bench", "pwmd", str(JSON_PATH)],
            cwd=BENCH_DIR, stdin=subprocess.PIPE, stdout=log, stderr=log)
        print(f"pwmd daemon 啟動 pid={self.proc.pid}(log: {DAEMON_LOG})")

    def stop(self):
        if self.proc is None:
            return
        try:
            self.proc.stdin.close()          # EOF → daemon 自己收攤
        except Exception:
            pass
        try:
            self.proc.wait(timeout=8)
            print("pwmd daemon 已收攤")
        except subprocess.TimeoutExpired:
            print("pwmd daemon 8 秒沒收攤,補 SIGINT(絕不 SIGKILL)")
            self.proc.send_signal(signal.SIGINT)
            try:
                self.proc.wait(timeout=8)
            except subprocess.TimeoutExpired:
                print("⚠ daemon 仍未退出,放著 —— 不 SIGKILL(MHU 會卡死)")
        self.proc = None

    @staticmethod
    def _read_json():
        try:
            return json.loads(JSON_PATH.read_text())
        except Exception:
            return None

    def send(self, gpt, ch, freq_hz, duty_permille, timeout=3.0):
        """寫一行指令,等 seq 前進,回傳 R8 的回報 dict。"""
        with self.lock:
            if not self.alive():
                # 節制的自動重啟:30 秒內只試一次,避免 thrash MHU
                now = time.monotonic()
                if now - self.last_restart < 30:
                    return {"ok": 0, "err": "daemon dead (recent restart failed)"}
                self.last_restart = now
                print("daemon 不在了,重啟一次…")
                self.start()
                time.sleep(2.5)              # 等 endpoint bind
                if not self.alive():
                    return {"ok": 0, "err": "daemon restart failed"}
            before = self._read_json()
            seq0 = before["seq"] if before else 0
            line = f"{gpt} {ch} {freq_hz} {duty_permille}\n"
            try:
                self.proc.stdin.write(line.encode())
                self.proc.stdin.flush()
            except Exception as e:
                return {"ok": 0, "err": f"stdin write: {e}"}
            deadline = time.monotonic() + timeout
            while time.monotonic() < deadline:
                cur = self._read_json()
                if cur and cur.get("seq", 0) > seq0:
                    return cur
                time.sleep(0.03)
            return {"ok": 0, "err": "timeout waiting daemon"}


DAEMON = Daemon()


class Handler(http.server.SimpleHTTPRequestHandler):
    def __init__(self, *a, **kw):
        super().__init__(*a, directory=str(HERE), **kw)

    def log_message(self, fmt, *args):     # 安靜點,錯誤照樣進 stderr
        pass

    def _json(self, code, obj):
        body = json.dumps(obj).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        if self.path in ("/", "/index.html"):
            self.path = "/pwm.html"
        if self.path == "/api/status":
            last = Daemon._read_json()
            return self._json(200, {"daemon_alive": DAEMON.alive(), "last": last})
        return super().do_GET()

    def do_POST(self):
        if self.path != "/api/pwm":
            return self._json(404, {"ok": 0, "err": "no such endpoint"})
        try:
            n = int(self.headers.get("Content-Length", "0"))
            req = json.loads(self.rfile.read(n))
            gpt = int(req["gpt"])
            ch = str(req["ch"]).lower()
            freq = int(req["freq_hz"])
            duty = int(req["duty_permille"])
        except Exception as e:
            return self._json(400, {"ok": 0, "err": f"bad request: {e}"})
        if gpt not in VALID_GPT or ch not in ("a", "b") \
           or not (1 <= freq <= 1_000_000) or not (0 <= duty <= 1000):
            return self._json(400, {"ok": 0, "err": "out of range"})
        rsp = DAEMON.send(gpt, ch, freq, duty)
        return self._json(200 if rsp.get("ok") else 502, rsp)


def main():
    if os.geteuid() != 0:
        sys.exit("要用 sudo 跑(daemon 開 /dev/uio 需要 root)")

    # 背景啟動(sh -c "... &")會繼承 SIGINT=ignore,Python 因此不裝
    # KeyboardInterrupt handler → 停不下來。明確裝回來,SIGTERM 也走同一條收攤路。
    def _sig(signum, frame):
        raise KeyboardInterrupt
    signal.signal(signal.SIGINT, _sig)
    signal.signal(signal.SIGTERM, _sig)

    class Srv(socketserver.ThreadingTCPServer):
        allow_reuse_address = True
        daemon_threads = True

    # 先綁 port 再起 daemon:port 被占時直接死在這裡,不會留下孤兒 daemon
    srv = Srv(("", PORT), Handler)
    DAEMON.start()
    print(f"PWM 示範網頁: http://<板子IP>:{PORT}/  (Ctrl-C 停止)")
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        print("\n收攤中…")
    finally:
        srv.server_close()
        DAEMON.stop()


if __name__ == "__main__":
    main()
