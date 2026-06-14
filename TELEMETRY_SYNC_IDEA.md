# Telemetry store-and-sync (idea)

## Problem

Telemetry is stream-only. If WiFi drops but the boat still has radio (RC),
all telemetry for that disconnected window is lost forever — the app only
logs what it receives live.

## Idea

Store telemetry onboard the boat and sync it to the app when the connection
returns. Likely shape: stream while WiFi is up; when WiFi is gone, buffer
onboard; on reconnect, sync the gap so the log/CSV is complete.

Bigger project for another day — not scoped or scheduled yet.

## Note

When this is built we likely want to **remove/trim some telemetry fields**.
Several are sent every frame but unused by the app, and a leaner payload
matters more once records are being buffered and synced.
