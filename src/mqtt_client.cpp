// #include <PubSubClient.h>
// #include <WiFi.h>

// extern String pendingJson;
// extern bool hasNewPayment;

// WiFiClient espClient;
// PubSubClient client(espClient);

// void mqttCallback(char* topic, byte* payload, unsigned int length) {

//     Serial.print("Message arrived [");
//     Serial.print(topic);
//     Serial.println("]");

//     String msg = "";

//     for (int i = 0; i < length; i++) {
//         msg += (char)payload[i];
//     }

//     Serial.println(msg);

//     pendingJson = msg;
//     hasNewPayment = true;
// }

// void mqttSetup() {

//     client.setServer("broker.hivemq.com", 1883);

//     client.setCallback(mqttCallback);   // QUAN TRỌNG
// }

// void mqttReconnect() {

//     static unsigned long lastTry = 0;

//     if (millis() - lastTry < 3000) return;

//     lastTry = millis();

//     Serial.println("Connecting MQTT...");

//     if (client.connect("ESP32_QR")) {

//         Serial.println("MQTT Connected");

//         client.subscribe("dqdatt/qr_payment");

//         Serial.println("Subscribe OK");
//     }
//     else {
//         Serial.print("MQTT Failed, state=");
//         Serial.println(client.state());
//     }
// }

// void mqttLoop() {

//     if (!client.connected()) {
//         mqttReconnect();
//     }

//     client.loop();
// }