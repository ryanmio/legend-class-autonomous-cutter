// useTelemetry.ts
// React hook that subscribes to the WebSocket telemetry stream.
// Re-renders the component when new data arrives (≥10 Hz from ESP32).

import { useState, useEffect } from 'react';
import { TelemetryData } from '../types';
import { subscribe, isConnected } from '../services/websocketService';

export function useTelemetry(): { data: TelemetryData | null; connected: boolean } {
  const [data, setData]           = useState<TelemetryData | null>(null);
  const [connected, setConnected] = useState(isConnected());

  useEffect(() => {
    const unsubscribe = subscribe((incoming) => {
      setData(incoming);
      setConnected(true);
    });

    // Poll connection state in case WS drops between frames
    const poll = setInterval(() => setConnected(isConnected()), 1000);

    return () => {
      unsubscribe();
      clearInterval(poll);
    };
  }, []);

  return { data, connected };
}
