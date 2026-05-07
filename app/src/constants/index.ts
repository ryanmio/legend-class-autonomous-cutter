// App-wide constants

// The ESP32 WiFi AP assigns itself this address
export const DEFAULT_BOAT_IP = '192.168.4.1';

export const HTTP_PORT = 80;

export const TELEMETRY_RECONNECT_MS = 2000;

// Dark nautical colour palette
export const Colors = {
  background:   '#0a0f1a',
  surface:      '#131b2e',
  surfaceLight: '#1c2840',
  accent:       '#00bfff',  // USCG blue
  accentOrange: '#ff6b00',
  danger:       '#ff3b30',
  warning:      '#ffcc00',
  success:      '#34c759',
  textPrimary:  '#e8f4fd',
  textSecondary:'#6b8fa8',
} as const;
