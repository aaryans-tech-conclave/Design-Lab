import json
import os
import socket
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib import parse, request

from google import genai

ESP_BASE = "http://192.168.4.1"
ARCHIVE_FILE = "esp_archive.json"
HANDLED_BOT_FILE = "handled_bot_requests.json"

POLL_INTERVAL = 5
REGISTER_INTERVAL = 20
SERVER_HOST = "0.0.0.0"
SERVER_PORT = 8080

BOT_SECRET = "BridgeBotSecret123"  # Must match the ESP sketch exactly.
GEMINI_MODEL = "gemini-3-flash-preview"

archive_lock = threading.Lock()
archive_data = []

GEMINI_API_KEY = os.getenv("GEMINI_API_KEY") or os.getenv("GOOGLE_API_KEY")
if not GEMINI_API_KEY:
    raise RuntimeError(
        "Missing Gemini API key. Set GEMINI_API_KEY before running this script."
    )

client = genai.Client(api_key=GEMINI_API_KEY)


def load_json_file(path, default):
    if not os.path.exists(path):
        return default
    with open(path, "r", encoding="utf-8") as f:
        return json.load(f)


def save_json_file(path, data):
    with open(path, "w", encoding="utf-8") as f:
        json.dump(data, f, indent=2, ensure_ascii=False)


def load_archive():
    return load_json_file(ARCHIVE_FILE, [])


def save_archive(data):
    save_json_file(ARCHIVE_FILE, data)


def load_handled_bot():
    return set(load_json_file(HANDLED_BOT_FILE, []))


def save_handled_bot(data):
    save_json_file(HANDLED_BOT_FILE, sorted(list(data)))


handled_bot_requests = load_handled_bot()


def fetch_messages():
    with request.urlopen(f"{ESP_BASE}/messages", timeout=5) as resp:
        return json.loads(resp.read().decode("utf-8"))


def normalize_messages(new_msgs):
    normalized = []
    for msg in new_msgs:
        key = msg.get("key")
        if not key:
            session = msg.get("session", "unknown")
            msg_id = msg.get("id", "0")
            key = f"{session}:{msg_id}"

        normalized.append({
            "id": msg.get("id", ""),
            "session": msg.get("session", ""),
            "key": key,
            "text": msg.get("text", "")
        })
    return normalized


def merge_messages(old_msgs, new_msgs):
    seen = {m["key"] for m in old_msgs}
    merged = old_msgs[:]
    for msg in new_msgs:
        if msg["key"] not in seen:
            merged.append(msg)
            seen.add(msg["key"])
    return merged


def get_local_ip_for_esp():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(("192.168.4.1", 80))
        return s.getsockname()[0]
    finally:
        s.close()


def register_with_esp():
    ip = get_local_ip_for_esp()
    bridge_url = f"http://{ip}:{SERVER_PORT}"
    body = parse.urlencode({"url": bridge_url}).encode("utf-8")
    req = request.Request(f"{ESP_BASE}/bridge/register", data=body, method="POST")
    with request.urlopen(req, timeout=5) as resp:
        _ = resp.read().decode("utf-8")
    return bridge_url


def ping_archive_refresh():
    body = parse.urlencode({"secret": BOT_SECRET}).encode("utf-8")
    req = request.Request(f"{ESP_BASE}/archive_ping", data=body, method="POST")
    with request.urlopen(req, timeout=5) as resp:
        return resp.read().decode("utf-8")


def post_bot_message(msg: str):
    body = parse.urlencode({
        "secret": BOT_SECRET,
        "msg": msg
    }).encode("utf-8")
    req = request.Request(f"{ESP_BASE}/api/bot_post", data=body, method="POST")
    with request.urlopen(req, timeout=10) as resp:
        return resp.read().decode("utf-8")


def extract_bot_question(full_text: str) -> str:
    lower = full_text.lower()
    idx = lower.find("@bot")
    if idx < 0:
        return ""
    question = full_text[idx + 4:].strip()
    return question


def ask_gemini(question: str) -> str:
    prompt = (
        "Answer the user's factual question directly. "
        "Do not use markdown. "
        "If the answer is a single word, number, or short expression, return only that. "
        "Otherwise answer in at most 2 short sentences and under 120 characters.\n\n"
        f"Question: {question}"
    )

    response = client.models.generate_content(
        model=GEMINI_MODEL,
        contents=prompt,
    )

    text = (response.text or "").strip()
    if not text:
        return "I could not answer that."
    if len(text) > 120:
        text = text[:120]
    return text


def process_bot_requests():
    global handled_bot_requests

    with archive_lock:
        data = archive_data[:]

    for msg in data:
        key = msg["key"]
        text = msg["text"]

        if key in handled_bot_requests:
            continue

        if "@bot" not in text.lower():
            continue

        question = extract_bot_question(text)
        if not question:
            handled_bot_requests.add(key)
            save_handled_bot(handled_bot_requests)
            continue

        try:
            answer = ask_gemini(question)
            post_bot_message(answer)
            print(f"[bot] answered: {question} -> {answer}")
        except Exception as e:
            print("[bot] failed:", e)

        handled_bot_requests.add(key)
        save_handled_bot(handled_bot_requests)


def sync_loop():
    global archive_data
    while True:
        try:
            current = normalize_messages(fetch_messages())

            with archive_lock:
                old_len = len(archive_data)
                archive_data = merge_messages(archive_data, current)
                save_archive(archive_data)
                new_len = len(archive_data)

            print(f"[sync] archived total messages: {len(archive_data)}")

            if new_len != old_len:
                try:
                    ping_archive_refresh()
                    print("[sync] archive refresh ping sent to ESP")
                except Exception as e:
                    print("[sync] archive ping failed:", e)

            process_bot_requests()

        except Exception as e:
            print("[sync] could not fetch from ESP:", e)

        time.sleep(POLL_INTERVAL)


def register_loop():
    while True:
        try:
            bridge_url = register_with_esp()
            print(f"[register] bridge registered at {bridge_url}")
        except Exception as e:
            print("[register] could not register with ESP:", e)
        time.sleep(REGISTER_INTERVAL)


def get_history(before_key, offset, limit):
    with archive_lock:
        data = archive_data[:]

    if not data:
        return [], False

    before_index = None
    for i, msg in enumerate(data):
        if msg["key"] == before_key:
            before_index = i
            break

    if before_index is None:
        return [], False

    older = data[:before_index]
    if not older:
        return [], False

    start_from_end = len(older) - offset
    if start_from_end < 0:
        start_from_end = 0

    end_from_end = max(start_from_end - limit, 0)
    chunk = older[end_from_end:start_from_end]
    has_more = end_from_end > 0

    return chunk, has_more


class BridgeHandler(BaseHTTPRequestHandler):
    def _send_json(self, code, payload):
        body = json.dumps(payload, ensure_ascii=False).encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        parsed = parse.urlparse(self.path)

        if parsed.path == "/status":
            with archive_lock:
                count = len(archive_data)
            self._send_json(200, {
                "ok": True,
                "archived_messages": count,
                "esp_base": ESP_BASE,
                "server_port": SERVER_PORT,
                "gemini_model": GEMINI_MODEL
            })
            return

        if parsed.path == "/history":
            qs = parse.parse_qs(parsed.query)
            before_key = qs.get("before_key", [""])[0]

            try:
                offset = int(qs.get("offset", ["0"])[0])
            except ValueError:
                offset = 0

            try:
                limit = int(qs.get("limit", ["10"])[0])
            except ValueError:
                limit = 10

            if offset < 0:
                offset = 0
            if limit < 1:
                limit = 1
            if limit > 10:
                limit = 10

            messages, has_more = get_history(before_key, offset, limit)
            self._send_json(200, {
                "ok": True,
                "messages": messages,
                "has_more": has_more
            })
            return

        self._send_json(404, {"ok": False, "error": "not found"})


if __name__ == "__main__":
    archive_data = load_archive()

    print("Starting bridge sync thread...")
    threading.Thread(target=sync_loop, daemon=True).start()

    print("Starting bridge register thread...")
    threading.Thread(target=register_loop, daemon=True).start()

    print(f"Bridge server listening on http://0.0.0.0:{SERVER_PORT}")
    print("Laptop must stay connected to LabBoard_Local")
    print("Archive file:", os.path.abspath(ARCHIVE_FILE))
    print("Handled bot file:", os.path.abspath(HANDLED_BOT_FILE))

    httpd = ThreadingHTTPServer((SERVER_HOST, SERVER_PORT), BridgeHandler)
    httpd.serve_forever()