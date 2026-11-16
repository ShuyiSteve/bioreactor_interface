// Optional MQTT bridge.
//
// Topics (per the Connectivity spec + course format):
//   <topic>/<deviceId>/telemetry   device -> server  (JSON telemetry)
//   <topic>/<deviceId>/commands    server -> device  (JSON command with id)
//   <topic>/<deviceId>/ack         device -> server  (command acknowledgement)
//   <topic>/<deviceId>/status       device -> server  ("online"/"offline" via LWT)
//
// Environment variables:
//   MQTT_URL         e.g. "mqtt://broker.hivemq.com:1883" (disabled if unset)
//   MQTT_TOPIC       topic prefix (default "bioreactor")
//   MQTT_DEVICE_ID   device id used for outgoing commands (default "bio1")
'use strict';

let client = null;
let topic = 'bioreactor';
let deviceId = 'bio1';
let onTelemetryCb = null;
let onAckCb = null;
let onStatusCb = null;

function start({ url, topicPrefix, commandDeviceId, onTelemetry, onAck, onStatus }) {
  if (!url) return null;

  topic = topicPrefix || topic;
  deviceId = commandDeviceId || deviceId;
  onTelemetryCb = onTelemetry;
  onAckCb = onAck;
  onStatusCb = onStatus;

  // Lazy require so HTTP-only deployments never load the MQTT dependency.
  // eslint-disable-next-line global-require
  const mqtt = require('mqtt');
  client = mqtt.connect(url, { reconnectPeriod: 3000 });

  client.on('connect', () => {
    client.subscribe(`${topic}/+/telemetry`, { qos: 0 });
    client.subscribe(`${topic}/+/ack`, { qos: 0 });
    client.subscribe(`${topic}/+/status`, { qos: 0 });
    console.log(`[mqtt] connected to ${url}, subscribed to ${topic}/+/telemetry, ${topic}/+/ack and ${topic}/+/status`);
  });

  client.on('message', (t, payload) => {
    const parts = t.split('/');
    const device = parts[1] || deviceId;
    const kind = parts[2]; // 'telemetry' | 'commands' | 'ack' | 'status'

    // The status topic carries a plain string ("online"/"offline"), not JSON.
    if (kind === 'status') {
      if (onStatusCb) onStatusCb(device, payload.toString().trim());
      return;
    }

    let msg;
    try {
      msg = JSON.parse(payload.toString());
    } catch (err) {
      console.error('[mqtt] invalid payload:', String(err));
      return;
    }

    if (kind === 'ack') {
      if (onAckCb) onAckCb(device, msg);
    } else {
      if (onTelemetryCb) onTelemetryCb(device, msg);
    }
  });

  client.on('error', (err) => console.error('[mqtt] error:', err.message));

  return client;
}

// Publish a command as { id, type, payload }.
function publishCommand(commandId, type, payload) {
  if (!client) return false;
  client.publish(`${topic}/${deviceId}/commands`, JSON.stringify({ id: commandId, type, payload }));
  return true;
}

function isEnabled() {
  return !!client;
}

module.exports = { start, publishCommand, isEnabled };
