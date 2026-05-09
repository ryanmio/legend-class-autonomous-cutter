import React, { useRef, useEffect, useState, useCallback } from 'react';
import { View, TouchableOpacity, Text, StyleSheet } from 'react-native';
import { WebView } from 'react-native-webview';
import { useSafeAreaInsets } from 'react-native-safe-area-context';
import { NativeStackScreenProps } from '@react-navigation/native-stack';
import { RootStackParamList } from '../../App';
import { Colors } from '../constants';
import { useTelemetry } from '../hooks/useTelemetry';
import { setWaypoint as sendWaypoint } from '../services/esp32Service';

type Props = NativeStackScreenProps<RootStackParamList, 'Map'>;

// ── Haversine bearing and distance ────────────────────────────────────────────
function bearingTo(fromLat: number, fromLon: number, toLat: number, toLon: number): number {
  const φ1 = fromLat * Math.PI / 180;
  const φ2 = toLat  * Math.PI / 180;
  const Δλ = (toLon - fromLon) * Math.PI / 180;
  const y = Math.sin(Δλ) * Math.cos(φ2);
  const x = Math.cos(φ1) * Math.sin(φ2) - Math.sin(φ1) * Math.cos(φ2) * Math.cos(Δλ);
  return (Math.atan2(y, x) * 180 / Math.PI + 360) % 360;
}

function distanceTo(fromLat: number, fromLon: number, toLat: number, toLon: number): number {
  const R  = 6371000;
  const φ1 = fromLat * Math.PI / 180;
  const φ2 = toLat   * Math.PI / 180;
  const Δφ = (toLat  - fromLat) * Math.PI / 180;
  const Δλ = (toLon  - fromLon) * Math.PI / 180;
  const a  = Math.sin(Δφ / 2) ** 2 + Math.cos(φ1) * Math.cos(φ2) * Math.sin(Δλ / 2) ** 2;
  return R * 2 * Math.atan2(Math.sqrt(a), Math.sqrt(1 - a));
}

// ── Leaflet map HTML ──────────────────────────────────────────────────────────
const MAP_HTML = `<!DOCTYPE html>
<html>
<head>
  <meta charset="utf-8"/>
  <meta name="viewport" content="width=device-width,initial-scale=1,maximum-scale=1,user-scalable=no"/>
  <link rel="stylesheet" href="https://unpkg.com/leaflet@1.9.4/dist/leaflet.css"/>
  <script src="https://unpkg.com/leaflet@1.9.4/dist/leaflet.js"></script>
  <style>
    *{margin:0;padding:0;box-sizing:border-box}
    body{background:#0a0f1a}
    #map{width:100vw;height:100vh}
    #hud{
      position:absolute;bottom:20px;left:12px;right:12px;
      background:rgba(10,15,26,0.88);color:#6b8fa8;
      padding:7px 14px;border-radius:8px;
      font:13px/1.6 monospace;z-index:1000;
      text-align:center;pointer-events:none
    }
    .boat{font-size:22px;line-height:1}
  </style>
</head>
<body>
  <div id="map"></div>
  <div id="hud">Waiting for GPS fix…</div>
  <script>
    var map = L.map('map',{zoomControl:true}).setView([37.8,-122.4],13);
    L.tileLayer('https://{s}.tile.openstreetmap.org/{z}/{x}/{y}.png',{
      attribution:'© OpenStreetMap contributors',maxZoom:19
    }).addTo(map);

    var boatIcon = L.divIcon({
      html:'<div class="boat">⛵</div>',
      iconSize:[28,28],iconAnchor:[14,14],className:''
    });
    var marker   = null;
    var wpMarker = null;
    var trail    = L.polyline([],{color:'#00bfff',weight:2.5,opacity:0.8}).addTo(map);
    var pts      = [];
    var locked   = false;

    window.updateBoat = function(lat, lon, hasFix, bearing, distM) {
      var hud = document.getElementById('hud');
      if (!hasFix) { hud.textContent = 'No GPS fix'; return; }
      var ll = [lat, lon];
      if (!marker) {
        marker = L.marker(ll,{icon:boatIcon}).addTo(map);
        map.setView(ll, 16);
      } else {
        marker.setLatLng(ll);
        if (locked) map.panTo(ll,{animate:true,duration:0.5});
      }
      pts.push(ll);
      if (pts.length > 800) pts.shift();
      trail.setLatLngs(pts);
      var posLine = lat.toFixed(6) + ',  ' + lon.toFixed(6);
      if (bearing != null && !isNaN(bearing)) {
        var d = distM >= 1000
          ? (distM / 1000).toFixed(2) + ' km'
          : Math.round(distM) + ' m';
        hud.innerHTML = posLine
          + '<br><span style="color:#00e676">→ WP: '
          + bearing.toFixed(1) + '° · ' + d + '</span>';
      } else {
        hud.textContent = posLine;
      }
    };

    window.centerOnBoat = function() {
      if (marker) { map.setView(marker.getLatLng(), map.getZoom()); locked = true; }
    };

    window.setWaypointMarker = function(lat, lon) {
      if (wpMarker) {
        wpMarker.setLatLng([lat, lon]);
      } else {
        wpMarker = L.circleMarker([lat, lon],{
          color:'#00e676',fillColor:'#00e676',fillOpacity:0.6,radius:9,weight:2
        }).addTo(map);
      }
    };

    window.clearWaypointMarker = function() {
      if (wpMarker) { map.removeLayer(wpMarker); wpMarker = null; }
    };

    map.on('dragstart', function() { locked = false; });

    // Tap anywhere on the map to drop a waypoint.
    map.on('click', function(e) {
      window.ReactNativeWebView.postMessage(JSON.stringify({
        type: 'waypoint',
        lat: e.latlng.lat,
        lon: e.latlng.lng
      }));
    });
  </script>
</body>
</html>`;

export default function MapScreen({ route, navigation }: Props) {
  const { ip } = route.params;
  const { data, connected } = useTelemetry();
  const webViewRef    = useRef<WebView>(null);
  const insets        = useSafeAreaInsets();
  const currentGpsRef = useRef<{ lat: number; lon: number } | null>(null);

  const [waypoint,  setWaypoint]  = useState<{ lat: number; lon: number } | null>(null);
  const [wpBearing, setWpBearing] = useState<number | null>(null);
  const [wpDistM,   setWpDistM]   = useState<number | null>(null);

  // Update boat marker and bearing HUD whenever GPS or waypoint changes.
  useEffect(() => {
    if (!webViewRef.current || !data) return;
    const lat    = parseFloat(data.lat ?? '');
    const lon    = parseFloat(data.lon ?? '');
    const hasFix = data.gps_fix === true && !isNaN(lat) && !isNaN(lon);

    if (hasFix) currentGpsRef.current = { lat, lon };

    let bearing: number | null = null;
    let dist:    number | null = null;
    if (hasFix && waypoint) {
      bearing = bearingTo(lat, lon, waypoint.lat, waypoint.lon);
      dist    = distanceTo(lat, lon, waypoint.lat, waypoint.lon);
      setWpBearing(bearing);
      setWpDistM(dist);
    }

    webViewRef.current.injectJavaScript(
      `window.updateBoat(${hasFix ? lat : 0},${hasFix ? lon : 0},${hasFix},` +
      `${bearing ?? 'null'},${dist ?? 'null'});true;`
    );
  }, [data?.gps_fix, data?.lat, data?.lon, waypoint]);

  // Handle tap-on-map messages from the WebView.
  const handleMapMessage = useCallback((msgData: string) => {
    try {
      const msg = JSON.parse(msgData);
      if (msg.type !== 'waypoint') return;
      const wpLat: number = msg.lat;
      const wpLon: number = msg.lon;

      const cur = currentGpsRef.current;
      const b   = cur ? bearingTo(cur.lat, cur.lon, wpLat, wpLon) : null;
      const d   = cur ? distanceTo(cur.lat, cur.lon, wpLat, wpLon) : null;

      setWaypoint({ lat: wpLat, lon: wpLon });
      if (b !== null) setWpBearing(b);
      if (d !== null) setWpDistM(d);

      // Drop the green marker and update the HUD immediately.
      webViewRef.current?.injectJavaScript(`window.setWaypointMarker(${wpLat},${wpLon});true;`);
      if (cur && b !== null && d !== null) {
        webViewRef.current?.injectJavaScript(
          `window.updateBoat(${cur.lat},${cur.lon},true,${b.toFixed(2)},${d.toFixed(0)});true;`
        );
      }

      // Tell the firmware (best-effort — don't block the UI on failure).
      sendWaypoint(ip, wpLat, wpLon).catch(() => {});
    } catch {}
  }, [ip]);

  const handleClearWaypoint = useCallback(() => {
    setWaypoint(null);
    setWpBearing(null);
    setWpDistM(null);
    webViewRef.current?.injectJavaScript('window.clearWaypointMarker();true;');
    // Revert HUD to just coordinates.
    const cur = currentGpsRef.current;
    if (cur) {
      webViewRef.current?.injectJavaScript(
        `window.updateBoat(${cur.lat},${cur.lon},true,null,null);true;`
      );
    }
    sendWaypoint(ip, null, null).catch(() => {});
  }, [ip]);

  return (
    <View style={styles.screen}>
      <WebView
        ref={webViewRef}
        source={{ html: MAP_HTML }}
        style={styles.map}
        originWhitelist={['*']}
        javaScriptEnabled
        domStorageEnabled
        onMessage={(e) => handleMapMessage(e.nativeEvent.data)}
      />

      <View style={[styles.topBar, { top: insets.top + 8 }]} pointerEvents="box-none">
        <TouchableOpacity style={styles.backBtn} onPress={() => navigation.goBack()}>
          <Text style={styles.backBtnText}>‹ BACK</Text>
        </TouchableOpacity>

        <View style={styles.connRow}>
          <Text style={[styles.dot, { color: connected ? Colors.success : Colors.danger }]}>●</Text>
          <Text style={styles.connText}>{connected ? 'CONNECTED' : 'OFFLINE'}</Text>
        </View>

        {waypoint && (
          <TouchableOpacity style={styles.clearBtn} onPress={handleClearWaypoint}>
            <Text style={styles.clearBtnText}>✕ WP</Text>
          </TouchableOpacity>
        )}

        <TouchableOpacity
          style={styles.centerBtn}
          onPress={() => webViewRef.current?.injectJavaScript('window.centerOnBoat();true;')}
        >
          <Text style={styles.centerBtnText}>CENTER</Text>
        </TouchableOpacity>
      </View>

      {/* Bearing readout — shown in native layer above the map when waypoint is active */}
      {wpBearing !== null && (
        <View style={[styles.bearingBar, { top: insets.top + 50 }]} pointerEvents="none">
          <Text style={styles.bearingText}>
            {'→ '}
            {wpBearing.toFixed(1)}°
            {wpDistM !== null
              ? `  ·  ${wpDistM >= 1000 ? (wpDistM / 1000).toFixed(2) + ' km' : Math.round(wpDistM) + ' m'}`
              : ''}
          </Text>
        </View>
      )}
    </View>
  );
}

const styles = StyleSheet.create({
  screen:      { flex: 1, backgroundColor: Colors.background },
  map:         { flex: 1 },
  topBar:      {
    position: 'absolute', left: 16, right: 16,
    flexDirection: 'row', justifyContent: 'space-between', alignItems: 'center',
  },
  backBtn:     { backgroundColor: 'rgba(10,15,26,0.85)', paddingHorizontal: 10, paddingVertical: 5, borderRadius: 12 },
  backBtnText: { color: Colors.textSecondary, fontSize: 11, fontFamily: 'monospace' },
  connRow:     { flexDirection: 'row', alignItems: 'center', backgroundColor: 'rgba(10,15,26,0.85)', paddingHorizontal: 10, paddingVertical: 5, borderRadius: 12 },
  dot:         { fontSize: 10, marginRight: 5 },
  connText:    { color: Colors.textSecondary, fontSize: 11, fontFamily: 'monospace' },
  clearBtn:    { backgroundColor: 'rgba(255,59,48,0.85)', paddingHorizontal: 10, paddingVertical: 5, borderRadius: 12 },
  clearBtnText:{ color: '#fff', fontSize: 11, fontFamily: 'monospace', fontWeight: 'bold' },
  centerBtn:   { backgroundColor: Colors.accent, paddingHorizontal: 14, paddingVertical: 7, borderRadius: 8 },
  centerBtnText:{ color: '#000', fontWeight: 'bold', fontSize: 12, letterSpacing: 1 },
  bearingBar:  {
    position: 'absolute', left: 16, right: 16, alignItems: 'center',
  },
  bearingText: {
    backgroundColor: 'rgba(0,230,118,0.15)',
    color: '#00e676',
    fontFamily: 'monospace',
    fontSize: 15,
    fontWeight: 'bold',
    paddingHorizontal: 16,
    paddingVertical: 4,
    borderRadius: 8,
    overflow: 'hidden',
  },
});
