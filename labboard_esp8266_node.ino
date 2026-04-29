#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClient.h>


const char* AP_SSID = "LabBoard_Local";
const char* AP_PASS = "LabBoard@123";
const char* NODE_ID = "NODE_1";


const char* BOT_SECRET = "BridgeBotSecret123";  // change this


ESP8266WebServer server(80);


const int MAX_MESSAGES = 40;
String messages[MAX_MESSAGES];
String messageIds[MAX_MESSAGES];
String messageKeys[MAX_MESSAGES];
int messageCount = 0;
unsigned long nextMessageId = 1;
String SESSION_ID;


// Bridge info
String bridgeBaseUrl = "";
unsigned long bridgeLastSeenMs = 0;
const unsigned long BRIDGE_TIMEOUT_MS = 120000; // 2 min


// Client ID management
const int MAX_CLIENTS = 10;
String clientTokens[MAX_CLIENTS];
int clientIds[MAX_CLIENTS];
bool clientActive[MAX_CLIENTS];
unsigned long clientLastSeen[MAX_CLIENTS];
const unsigned long CLIENT_TIMEOUT_MS = 30UL * 60UL * 1000UL; // 30 min


// Debug
unsigned long lastClientPrintMs = 0;
int lastPrintedClientCount = -1;


String htmlEscape(const String& s) {
  String out;
  out.reserve(s.length() + 16);
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == '&') out += "&amp;";
    else if (c == '<') out += "&lt;";
    else if (c == '>') out += "&gt;";
    else if (c == '"') out += "&quot;";
    else if (c == '\'') out += "&#39;";
    else out += c;
  }
  return out;
}


String jsonEscape(const String& s) {
  String out;
  out.reserve(s.length() + 16);
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == '\\') out += "\\\\";
    else if (c == '"') out += "\\\"";
    else if (c == '\n') out += "\\n";
    else if (c == '\r') out += "\\r";
    else if (c == '\t') out += "\\t";
    else out += c;
  }
  return out;
}


bool bridgeIsFresh() {
  if (bridgeBaseUrl.length() == 0) return false;
  return (millis() - bridgeLastSeenMs) <= BRIDGE_TIMEOUT_MS;
}


bool isReservedAuthor(const String& author) {
  return author.equalsIgnoreCase("bot");
}


void cleanupInactiveClients() {
  unsigned long now = millis();
  for (int i = 0; i < MAX_CLIENTS; i++) {
    if (clientActive[i] && (now - clientLastSeen[i] > CLIENT_TIMEOUT_MS)) {
      clientActive[i] = false;
      clientTokens[i] = "";
      clientIds[i] = 0;
      clientLastSeen[i] = 0;
    }
  }
}


int findLowestFreeId() {
  for (int candidate = 1; candidate <= MAX_CLIENTS; candidate++) {
    bool used = false;
    for (int i = 0; i < MAX_CLIENTS; i++) {
      if (clientActive[i] && clientIds[i] == candidate) {
        used = true;
        break;
      }
    }
    if (!used) return candidate;
  }
  return -1;
}


int getOrAssignClientId(const String& token) {
  cleanupInactiveClients();


  for (int i = 0; i < MAX_CLIENTS; i++) {
    if (clientActive[i] && clientTokens[i] == token) {
      clientLastSeen[i] = millis();
      return clientIds[i];
    }
  }


  int freeId = findLowestFreeId();
  if (freeId < 0) return -1;


  for (int i = 0; i < MAX_CLIENTS; i++) {
    if (!clientActive[i]) {
      clientActive[i] = true;
      clientTokens[i] = token;
      clientIds[i] = freeId;
      clientLastSeen[i] = millis();
      return freeId;
    }
  }


  return -1;
}


void addMessage(const String& authorRaw, const String& textRaw) {
  String author = authorRaw;
  String text = textRaw;


  author.trim();
  text.trim();


  if (author.length() == 0) author = "Anonymous";
  if (author.length() > 24) author = author.substring(0, 24);
  if (text.length() == 0) return;
  if (text.length() > 180) text = text.substring(0, 180);


  String entry = "[" + String(millis() / 1000) + "s] " + author + ": " + text;
  String id = String(nextMessageId++);
  String key = SESSION_ID + ":" + id;


  if (messageCount < MAX_MESSAGES) {
    messages[messageCount] = entry;
    messageIds[messageCount] = id;
    messageKeys[messageCount] = key;
    messageCount++;
  } else {
    for (int i = 1; i < MAX_MESSAGES; i++) {
      messages[i - 1] = messages[i];
      messageIds[i - 1] = messageIds[i];
      messageKeys[i - 1] = messageKeys[i];
    }
    messages[MAX_MESSAGES - 1] = entry;
    messageIds[MAX_MESSAGES - 1] = id;
    messageKeys[MAX_MESSAGES - 1] = key;
  }
}


String getMessagesHtml() {
  String out;
  if (messageCount == 0) return "<li>No messages yet.</li>";


  for (int i = messageCount - 1; i >= 0; i--) {
    out += "<li><b>#";
    out += messageIds[i];
    out += "</b> ";
    out += htmlEscape(messages[i]);
    out += "</li>";
  }
  return out;
}


String getMessagesJson() {
  String json = "[";
  for (int i = 0; i < messageCount; i++) {
    if (i > 0) json += ",";
    json += "{";
    json += "\"id\":\"" + jsonEscape(messageIds[i]) + "\",";
    json += "\"session\":\"" + jsonEscape(SESSION_ID) + "\",";
    json += "\"key\":\"" + jsonEscape(messageKeys[i]) + "\",";
    json += "\"text\":\"" + jsonEscape(messages[i]) + "\"";
    json += "}";
  }
  json += "]";
  return json;
}


String buildPage() {
  String page;
  page.reserve(22000);


  page += R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width,initial-scale=1">
  <title>ESP Announcement Board</title>
  <style>
    body { font-family: Arial, sans-serif; margin: 20px; background: #f4f6fb; color: #222; }
    .box { background: white; border-radius: 12px; padding: 16px; margin-bottom: 16px; box-shadow: 0 2px 10px rgba(0,0,0,0.08); }
    h1, h2 { margin-top: 0; }
    input[type=text], textarea {
      width: 100%;
      box-sizing: border-box;
      padding: 12px;
      margin-top: 6px;
      margin-bottom: 12px;
      border: 1px solid #ccc;
      border-radius: 8px;
      font-size: 14px;
    }
    textarea { resize: vertical; min-height: 80px; }
    button {
      padding: 10px 16px;
      border: 0;
      border-radius: 8px;
      cursor: pointer;
      background: #2d6cdf;
      color: white;
      font-size: 14px;
      margin-right: 8px;
      margin-bottom: 8px;
    }
    .secondary { background: #666; }
    .danger { background: #b44; }
    .mono { font-family: monospace; }
    .small { color: #555; font-size: 14px; }
    .status { margin-top: 8px; color: #444; font-size: 14px; }
    ul { padding-left: 20px; }
    .hidden { display: none; }
  </style>
</head>
<body>
)rawliteral";


  page += "<div class='box'>";
  page += "<h1>ESP Announcement Board</h1>";
  page += "<p class='small'>Hosted directly on the ESP8266 hotspot.</p>";
  page += "<button class='danger' onclick='logoutClient()'>Logout this client</button>";
  page += "</div>";


  page += "<div class='box'>";
  page += "<h2>Network Status</h2>";
  page += "<p><b>Node ID:</b> " + htmlEscape(String(NODE_ID)) + "</p>";
  page += "<p><b>Session ID:</b> <span class='mono'>" + htmlEscape(SESSION_ID) + "</span></p>";
  page += "<p><b>Hotspot SSID:</b> " + htmlEscape(String(AP_SSID)) + "</p>";
  page += "<p><b>Hotspot IP:</b> <span class='mono'>" + WiFi.softAPIP().toString() + "</span></p>";
  page += "<p><b>Connected clients:</b> " + String(WiFi.softAPgetStationNum()) + "</p>";
  page += "<p><b>Stored messages on ESP:</b> " + String(messageCount) + " / " + String(MAX_MESSAGES) + "</p>";
  page += "<p><b>Bridge status:</b> ";
  page += bridgeIsFresh() ? "Connected" : "Not connected";
  page += "</p>";
  page += "<p><b>Bridge URL:</b> <span class='mono'>" + htmlEscape(bridgeBaseUrl) + "</span></p>";
  page += "<p><b>Free heap:</b> " + String(ESP.getFreeHeap()) + " bytes</p>";
  page += "<p><b>Your client ID:</b> <span id='clientIdLabel'>unknown</span></p>";
  page += "</div>";


  page += R"rawliteral(
<div class="box">
  <h2>Post Message</h2>
  <form action="/post" method="POST" onsubmit="return beforeSubmit()">
    <input type="hidden" name="client_token" id="clientToken">
    <label>Your name</label>
    <input type="text" name="author" id="authorInput" maxlength="24" placeholder="Enter your name">
    <label>Message</label>
    <textarea name="msg" maxlength="180" placeholder="Write an announcement or message"></textarea>
    <button type="submit">Post</button>
  </form>
</div>


<div class="box">
  <h2>History</h2>
  <div>
    <button onclick="showHistory()">Show history</button>
    <button class="secondary" onclick="loadOlder()">Load older</button>
    <button class="danger" onclick="hideHistory()">Hide history</button>
  </div>
  <div id="historyStatus" class="status">History is hidden.</div>
</div>


<div class="box hidden" id="historyBox">
  <h2>Older Archived Messages</h2>
  <ul id="historyList"></ul>
</div>
)rawliteral";


  page += "<div class='box'><h2>Latest Messages on ESP</h2><ul>";
  page += getMessagesHtml();
  page += "</ul></div>";


  page += R"rawliteral(
<script>
let historyOffset = 0;


function makeToken() {
  return 'tok_' + Math.random().toString(36).slice(2) + '_' + Date.now().toString(36);
}


async function ensureClientIdentity() {
  let token = localStorage.getItem('clientToken');
  if (!token) {
    token = makeToken();
    localStorage.setItem('clientToken', token);
  }


  document.getElementById('clientToken').value = token;


  const body = new URLSearchParams();
  body.set('client_token', token);


  const resp = await fetch('/register_client', {
    method: 'POST',
    headers: {'Content-Type': 'application/x-www-form-urlencoded'},
    body: body.toString()
  });


  const data = await resp.json();
  if (data.ok) {
    localStorage.setItem('clientId', data.client_id);
    document.getElementById('clientIdLabel').textContent = data.client_id;
  } else {
    document.getElementById('clientIdLabel').textContent = 'error';
  }
}


async function logoutClient() {
  const token = localStorage.getItem('clientToken');
  if (!token) return;


  const body = new URLSearchParams();
  body.set('client_token', token);


  await fetch('/logout_client', {
    method: 'POST',
    headers: {'Content-Type': 'application/x-www-form-urlencoded'},
    body: body.toString()
  });


  localStorage.removeItem('clientToken');
  localStorage.removeItem('clientId');
  location.reload();
}


function beforeSubmit() {
  const token = localStorage.getItem('clientToken');
  document.getElementById('clientToken').value = token || '';
  return true;
}


async function fetchHistoryChunk(offset) {
  const resp = await fetch('/history?offset=' + encodeURIComponent(offset) + '&limit=10');
  const data = await resp.json();
  if (!resp.ok || !data.ok) {
    throw new Error(data.error || 'History fetch failed');
  }
  return data;
}


function setStatus(text) {
  document.getElementById('historyStatus').textContent = text;
}


function appendHistory(msgs) {
  const list = document.getElementById('historyList');
  if (msgs.length === 0 && list.children.length === 0) {
    const li = document.createElement('li');
    li.textContent = 'No older messages found.';
    list.appendChild(li);
    return;
  }


  msgs.forEach(msg => {
    const li = document.createElement('li');
    li.innerHTML = '<b>#' + msg.id + '</b> ' + escapeHtml(msg.text);
    list.prepend(li);
  });
}


async function showHistory() {
  try {
    historyOffset = 0;
    document.getElementById('historyList').innerHTML = '';
    const historyBox = document.getElementById('historyBox');
    historyBox.classList.remove('hidden');


    const data = await fetchHistoryChunk(historyOffset);
    appendHistory(data.messages);
    historyOffset += data.messages.length;


    if (data.messages.length === 0) {
      setStatus('No older archived messages available.');
    } else {
      setStatus(data.has_more ? 'Showing first older 10 messages.' : 'Showing all available older messages.');
    }
  } catch (e) {
    setStatus('Could not fetch history: ' + e.message);
  }
}


async function loadOlder() {
  try {
    if (document.getElementById('historyBox').classList.contains('hidden')) {
      await showHistory();
      return;
    }
    const data = await fetchHistoryChunk(historyOffset);
    appendHistory(data.messages);
    historyOffset += data.messages.length;


    if (data.messages.length === 0) {
      setStatus('No more older messages available.');
    } else {
      setStatus(data.has_more ? 'Loaded 10 more older messages.' : 'Loaded final older messages.');
    }
  } catch (e) {
    setStatus('Could not load older history: ' + e.message);
  }
}


function hideHistory() {
  document.getElementById('historyBox').classList.add('hidden');
  document.getElementById('historyList').innerHTML = '';
  historyOffset = 0;
  setStatus('History is hidden.');
}


function escapeHtml(s) {
  return s.replaceAll('&', '&amp;')
          .replaceAll('<', '&lt;')
          .replaceAll('>', '&gt;')
          .replaceAll('"', '&quot;')
          .replaceAll("'", '&#39;');
}


window.addEventListener('load', ensureClientIdentity);
</script>
)rawliteral";


  page += "</body></html>";
  return page;
}


void handleRoot() {
  server.send(200, "text/html", buildPage());
}


void handleRegisterClient() {
  String token = server.hasArg("client_token") ? server.arg("client_token") : "";
  token.trim();


  if (token.length() == 0) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"missing client_token\"}");
    return;
  }


  int clientId = getOrAssignClientId(token);
  if (clientId < 0) {
    server.send(503, "application/json", "{\"ok\":false,\"error\":\"no free client ids\"}");
    return;
  }


  String json = "{";
  json += "\"ok\":true,";
  json += "\"client_id\":" + String(clientId);
  json += "}";
  server.send(200, "application/json", json);
}


void handleLogoutClient() {
  String token = server.hasArg("client_token") ? server.arg("client_token") : "";
  token.trim();


  if (token.length() == 0) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"missing client_token\"}");
    return;
  }


  for (int i = 0; i < MAX_CLIENTS; i++) {
    if (clientActive[i] && clientTokens[i] == token) {
      clientActive[i] = false;
      clientTokens[i] = "";
      clientIds[i] = 0;
      clientLastSeen[i] = 0;
      server.send(200, "application/json", "{\"ok\":true}");
      return;
    }
  }


  server.send(200, "application/json", "{\"ok\":true}");
}


void handlePost() {
  String author = server.hasArg("author") ? server.arg("author") : "";
  String msg = server.hasArg("msg") ? server.arg("msg") : "";
  String token = server.hasArg("client_token") ? server.arg("client_token") : "";


  token.trim();
  author.trim();
  msg.trim();


  if (token.length() == 0) {
    server.send(400, "text/plain", "Missing client token");
    return;
  }


  int clientId = getOrAssignClientId(token);
  if (clientId < 0) {
    server.send(503, "text/plain", "No free client IDs");
    return;
  }


  if (isReservedAuthor(author)) {
    server.send(403, "text/plain", "Reserved author name");
    return;
  }


  if (author.length() == 0) author = "Anonymous";
  if (author.length() > 24) author = author.substring(0, 24);


  String finalAuthor = author + "#" + String(clientId);


  Serial.print("POST received from author='");
  Serial.print(finalAuthor);
  Serial.print("' msg='");
  Serial.print(msg);
  Serial.println("'");


  addMessage(finalAuthor, msg);


  server.sendHeader("Location", "/", true);
  server.send(303, "text/plain", "OK");
}


void handleBotPost() {
  String secret = server.hasArg("secret") ? server.arg("secret") : "";
  String msg = server.hasArg("msg") ? server.arg("msg") : "";


  if (secret != BOT_SECRET) {
    server.send(403, "application/json", "{\"ok\":false,\"error\":\"forbidden\"}");
    return;
  }


  msg.trim();
  if (msg.length() == 0) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"missing msg\"}");
    return;
  }


  if (msg.length() > 120) msg = msg.substring(0, 120);


  Serial.print("BOT POST msg='");
  Serial.print(msg);
  Serial.println("'");


  addMessage("Bot", msg);
  server.send(200, "application/json", "{\"ok\":true}");
}


void handleMessages() {
  server.send(200, "application/json", getMessagesJson());
}


void handleStatus() {
  String json = "{";
  json += "\"ok\":true,";
  json += "\"node_id\":\"" + jsonEscape(String(NODE_ID)) + "\",";
  json += "\"session_id\":\"" + jsonEscape(SESSION_ID) + "\",";
  json += "\"ap_ssid\":\"" + jsonEscape(String(AP_SSID)) + "\",";
  json += "\"ap_ip\":\"" + WiFi.softAPIP().toString() + "\",";
  json += "\"ap_clients\":" + String(WiFi.softAPgetStationNum()) + ",";
  json += "\"free_heap\":" + String(ESP.getFreeHeap()) + ",";
  json += "\"message_count\":" + String(messageCount) + ",";
  json += "\"bridge_connected\":" + String(bridgeIsFresh() ? "true" : "false") + ",";
  json += "\"bridge_url\":\"" + jsonEscape(bridgeBaseUrl) + "\"";
  json += "}";
  server.send(200, "application/json", json);
}


void handleBridgeRegister() {
  String url = server.hasArg("url") ? server.arg("url") : "";
  url.trim();


  if (url.length() == 0) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"missing url\"}");
    return;
  }


  bridgeBaseUrl = url;
  bridgeLastSeenMs = millis();


  Serial.print("Bridge registered: ");
  Serial.println(bridgeBaseUrl);


  server.send(200, "application/json", "{\"ok\":true}");
}


void handleHistory() {
  if (!bridgeIsFresh()) {
    server.send(503, "application/json", "{\"ok\":false,\"error\":\"bridge not available\"}");
    return;
  }


  if (messageCount == 0) {
    server.send(200, "application/json", "{\"ok\":true,\"messages\":[],\"has_more\":false}");
    return;
  }


  String beforeKey = messageKeys[0];
  int offset = server.hasArg("offset") ? server.arg("offset").toInt() : 0;
  int limit  = server.hasArg("limit")  ? server.arg("limit").toInt()  : 10;


  if (offset < 0) offset = 0;
  if (limit < 1) limit = 1;
  if (limit > 10) limit = 10;


  String url = bridgeBaseUrl + "/history?before_key=" + beforeKey +
               "&offset=" + String(offset) +
               "&limit=" + String(limit);


  WiFiClient client;
  HTTPClient http;


  Serial.print("Proxying history request to bridge: ");
  Serial.println(url);


  if (!http.begin(client, url)) {
    server.send(500, "application/json", "{\"ok\":false,\"error\":\"could not start bridge request\"}");
    return;
  }


  int httpCode = http.GET();
  if (httpCode <= 0) {
    http.end();
    server.send(502, "application/json", "{\"ok\":false,\"error\":\"bridge request failed\"}");
    return;
  }


  String payload = http.getString();
  http.end();


  server.send(httpCode, "application/json", payload);
}


void handleNotFound() {
  server.send(404, "text/plain", "Not found");
}


void setup() {
  Serial.begin(115200);
  delay(500);


  SESSION_ID = String(ESP.getChipId(), HEX) + "-" + String(ESP.getCycleCount(), HEX);


  Serial.println();
  Serial.println("Starting ESP Announcement Board...");


  WiFi.mode(WIFI_AP);
  bool ok = WiFi.softAP(AP_SSID, AP_PASS);


  if (ok) {
    Serial.println("Hotspot started successfully");
    Serial.print("Node ID: ");
    Serial.println(NODE_ID);
    Serial.print("Session ID: ");
    Serial.println(SESSION_ID);
    Serial.print("SSID: ");
    Serial.println(AP_SSID);
    Serial.print("Password: ");
    Serial.println(AP_PASS);
    Serial.print("Open in browser: http://");
    Serial.println(WiFi.softAPIP());
    Serial.print("Connected AP clients: ");
    Serial.println(WiFi.softAPgetStationNum());
  } else {
    Serial.println("Hotspot start failed");
  }


  addMessage("System", "Board started");
  addMessage("System", "Hotspot is ready");
  addMessage("System", "Bridge laptop can register and serve history");


  server.on("/", HTTP_GET, handleRoot);
  server.on("/post", HTTP_POST, handlePost);
  server.on("/messages", HTTP_GET, handleMessages);
  server.on("/status", HTTP_GET, handleStatus);
  server.on("/bridge/register", HTTP_POST, handleBridgeRegister);
  server.on("/history", HTTP_GET, handleHistory);
  server.on("/register_client", HTTP_POST, handleRegisterClient);
  server.on("/logout_client", HTTP_POST, handleLogoutClient);
  server.on("/api/bot_post", HTTP_POST, handleBotPost);
  server.onNotFound(handleNotFound);
  server.begin();


  Serial.println("Web server started");
}


void loop() {
  server.handleClient();


  unsigned long now = millis();
  if (now - lastClientPrintMs >= 2000) {
    lastClientPrintMs = now;
    int currentClients = WiFi.softAPgetStationNum();


    if (currentClients != lastPrintedClientCount) {
      lastPrintedClientCount = currentClients;
      Serial.print("Connected AP clients: ");
      Serial.println(currentClients);
    }
  }
}

