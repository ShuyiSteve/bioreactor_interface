// Prints every MQTT message under bioreactor/<device>/# with a timestamp.
// Usage: npm run monitor            (uses MQTT_URL / MQTT_TOPIC / MQTT_DEVICE_ID from .env)
//        npm run monitor -- mqtt://test.mosquitto.org:1883
'use strict';

require('dotenv').config();
const mqtt = require('mqtt');

const url = process.argv[2] || process.env.MQTT_URL || 'mqtt://broker.hivemq.com:1883';
const filter = `${process.env.MQTT_TOPIC || 'bioreactor'}/${process.env.MQTT_DEVICE_ID || 'bio1'}/#`;

const client = mqtt.connect(url, { reconnectPeriod: 3000 });
client.on('connect', () => {
  client.subscribe(filter);
  console.log(`[monitor] connected to ${url}, listening on ${filter}`);
});
client.on('message', (topic, payload, packet) => {
  const retained = packet.retain ? ' (retained)' : '';
  console.log(`${new Date().toISOString()} ${topic}${retained} ${payload.toString()}`);
});
client.on('error', (err) => console.error('[monitor] error:', err.message));
