// Optional MQTT bridge.
//
// Per the Connectivity spec, the ESP32 DevKitC bridge publishes telemetry to
// `<topic>/<deviceId>/telemetry` and subscribes to `<topic>/<deviceId>/commands`.
// The server mirrors that: it subscribes to telemetry from any device and
// publishes operator commands back to the configured device.
//
// Environment variables:
//   MQTT_URL         e.g. "mqtt://broker.hivemq.com:1883" (disabled if unset)
//   MQTT_TOPIC       topic prefix (default "bioreactor")
//   MQTT_DEVICE_ID   device id used for outgoing commands (default "bio1")
'use strict';

let client = null;
let topic = 'bioreactor';
let deviceId = 'bio1';

function start({ url, topicPrefix, commandDeviceId, onTelemetry }) {
  if (!url) return null;

  topic = topicPrefix || topic;
  deviceId = commandDeviceId || deviceId;

  // Lazy require so HTTP-only deployments never load the MQTT dependency.
  // eslint-disable-next-line global-require
  const mqtt = require('mqtt');
  client = mqtt.connect(url, { reconnectPeriod: 3000 });

  client.on('connect', () => {
    client.subscribe(`${topic}/+/telemetry`, { qos: 0 });
    console.log(`[mqtt] connected to ${url}, subscribed to ${topic}/+/telemetry`);
  });

  client.on('message', (t, payload) => {
    try {
      const msg = JSON.parse(payload.toString());
      const device = t.split('/')[1] || deviceId;
      onTelemetry(device, msg);
    } catch (err) {
      console.error('[mqtt] invalid telemetry payload:', String(err));
    }
  });

  client.on('error', (err) => console.error('[mqtt] error:', err.message));

  return client;
}

function publishCommand(type, payload) {
  if (!client) return false;
  client.publish(`${topic}/${deviceId}/commands`, JSON.stringify({ type, payload }));
  return true;
}

function isEnabled() {
  return !!client;
}

module.exports = { start, publishCommand, isEnabled };
