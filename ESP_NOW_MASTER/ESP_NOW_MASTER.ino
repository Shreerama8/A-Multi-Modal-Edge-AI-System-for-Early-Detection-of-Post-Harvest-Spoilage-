#include <esp_now.h>
#include <WiFi.h>
//B4:3A:45:F3:9D:D4
uint8_t broadcastAddress[] = {0xB4, 0x3A, 0x45, 0xF3, 0x9D, 0xD4};

char msg[] = "WELCOME TO PRONETELECTRONICS!";

esp_now_peer_info_t peerInfo;

void OnDataSent(const uint8_t *mac_addr, esp_now_send_status_t status) {
  Serial.print("\r\nDelivery Status: ");
  Serial.println(status == ESP_NOW_SEND_SUCCESS ? "Deliverd Successfully" : "Delivery Fail");
}
 
void setup() {
  Serial.begin(115200);
  WiFi.mode(WIFI_STA);
  if (esp_now_init() != ESP_OK) {
    Serial.println("Error initializing ESP-NOW");
    return;
  }

  esp_now_register_send_cb(OnDataSent);
  memcpy(peerInfo.peer_addr, broadcastAddress, 6);
  peerInfo.channel = 0; 
  peerInfo.encrypt = false;
         
  if (esp_now_add_peer(&peerInfo) != ESP_OK){
    Serial.println("Failed to add peer");
    return;
  }
}
 
void loop() {
  esp_err_t result = esp_now_send(broadcastAddress, (uint8_t *) &msg, sizeof(msg));

  if (result == ESP_OK) {
    Serial.println("Sent Successfully");
  }
  else {
    Serial.println("Error while sending data");
  }
  delay(1000);
}
