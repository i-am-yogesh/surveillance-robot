#ifndef TELEGRAM_API_H
#define TELEGRAM_API_H

#include <WiFiClientSecure.h>

// ─── Config ───────────────────────────────────────
#define BOT_TOKEN  "123456789:ABCDEFGH"
#define CHAT_ID    "123456789"
#define TELEGRAM_HOST "api.telegram.org"
// ──────────────────────────────────────────────────

bool sendTelegramMessage(const char* message) {
  WiFiClientSecure client;
  client.setInsecure(); // skip SSL cert validation (fine for IoT)
  client.setTimeout(15);

  if (!client.connect(TELEGRAM_HOST, 443)) {
    Serial.println("[Telegram] Connection failed");
    return false;
  }

  String url = "/bot" + String(BOT_TOKEN) + "/sendMessage";
  String body = "chat_id=" + String(CHAT_ID) +
                "&text=" + String(message) +
                "&parse_mode=Markdown";

  client.println("POST " + url + " HTTP/1.1");
  client.println("Host: " + String(TELEGRAM_HOST));
  client.println("Content-Type: application/x-www-form-urlencoded");
  client.println("Content-Length: " + String(body.length()));
  client.println("Connection: close");
  client.println();
  client.print(body);

  // Wait for response
  long timeout = millis();
  while (client.available() == 0) {
    if (millis() - timeout > 10000) {
      Serial.println("[Telegram] Timeout");
      client.stop();
      return false;
    }
  }

  String response = client.readString();
  client.stop();
  return response.indexOf("\"ok\":true") > 0;
}

#endif