// websocketService.ts
// Persistent WebSocket connection to ESP32 telemetry stream.
// Reconnects automatically on disconnect.
// Call connect() once; subscribe() to receive parsed TelemetryData at ~10 Hz.

import { WS_PORT, TELEMETRY_RECONNECT_MS } from '../constants';
import { TelemetryData } from '../types';

type Listener = (data: TelemetryData) => void;

let ws: WebSocket | null = null;
let reconnectTimer: ReturnType<typeof setTimeout> | null = null;
const listeners = new Set<Listener>();
let currentIP = '';

function scheduleReconnect() {
  if (reconnectTimer) return;
  reconnectTimer = setTimeout(() => {
    reconnectTimer = null;
    if (currentIP) connect(currentIP);
  }, TELEMETRY_RECONNECT_MS);
}

export function connect(ip: string) {
  currentIP = ip;
  if (ws) {
    ws.onclose = null;
    ws.close();
    ws = null;
  }

  ws = new WebSocket(`ws://${ip}:${WS_PORT}`);

  ws.onopen = () => {
    console.log('[WS] Connected to', ip);
  };

  ws.onmessage = (event) => {
    try {
      const data: TelemetryData = JSON.parse(event.data as string);
      listeners.forEach((fn) => fn(data));
    } catch {
      // Malformed frame — ignore
    }
  };

  ws.onerror = () => {
    ws?.close();
  };

  ws.onclose = () => {
    ws = null;
    scheduleReconnect();
  };
}

export function disconnect() {
  currentIP = '';
  if (reconnectTimer) { clearTimeout(reconnectTimer); reconnectTimer = null; }
  if (ws) { ws.onclose = null; ws.close(); ws = null; }
}

export function subscribe(fn: Listener): () => void {
  listeners.add(fn);
  return () => listeners.delete(fn);
}

export function isConnected(): boolean {
  return ws?.readyState === WebSocket.OPEN;
}
