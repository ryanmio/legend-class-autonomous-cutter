// useTelemetry.ts
// React hook that subscribes to websocketService's telemetry updates (an
// HTTP poll roughly every 1 s, despite the module name — see its header).
// Re-renders the component whenever a new frame arrives.

import { useState, useEffect } from 'react';
import { TelemetryData } from '../types';
import { subscribe, isConnected, getLastData } from '../services/websocketService';

export function useTelemetry(): { data: TelemetryData | null; connected: boolean } {
  // Initialize from the websocket-service cache so screens that remount
  // (e.g. Helm ↔ Map) don't briefly show empty state while waiting for
  // the next poll. The next poll still arrives within ~1s and overrides.
  const [data, setData]           = useState<TelemetryData | null>(getLastData());
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
