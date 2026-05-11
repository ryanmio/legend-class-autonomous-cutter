import React, { useRef, useEffect, useState, useCallback } from 'react';
import { View, TouchableOpacity, Text, StyleSheet } from 'react-native';
import { WebView } from 'react-native-webview';
import { useSafeAreaInsets } from 'react-native-safe-area-context';
import { NativeStackScreenProps } from '@react-navigation/native-stack';
import { RootStackParamList } from '../../App';
import { Colors } from '../constants';
import { useTelemetry } from '../hooks/useTelemetry';
import { setWaypoint as sendWaypoint, setCruise as sendCruise } from '../services/esp32Service';
import { CruiseModal } from '../components/CruiseModal';

type Props = NativeStackScreenProps<RootStackParamList, 'Map'>;

// ── Haversine bearing and distance (used for the local HUD overlay) ───────────
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

// ── Leaflet map HTML (unchanged) ──────────────────────────────────────────────
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

// ── Mode pill colours ─────────────────────────────────────────────────────────
function modeColor(mode: string | undefined): string {
  switch (mode) {
    case 'AUTO':     return Colors.success;
    case 'MANUAL':   return Colors.accent;
    case 'FAILSAFE': return Colors.danger;
    default:         return Colors.textSecondary;
  }
}

// ── Screen ────────────────────────────────────────────────────────────────────
export default function MapScreen({ route, navigation }: Props) {
  const { ip } = route.params;
  const { data, connected } = useTelemetry();
  const webViewRef    = useRef<WebView>(null);
  const insets        = useSafeAreaInsets();
  const currentGpsRef = useRef<{ lat: number; lon: number } | null>(null);

  const [waypoint,  setWaypoint]  = useState<{ lat: number; lon: number } | null>(null);
  const [wpBearing, setWpBearing] = useState<number | null>(null);
  const [wpDistM,   setWpDistM]   = useState<number | null>(null);
  const [cruiseModalOpen, setCruiseModalOpen] = useState(false);

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

      webViewRef.current?.injectJavaScript(`window.setWaypointMarker(${wpLat},${wpLon});true;`);
      if (cur && b !== null && d !== null) {
        webViewRef.current?.injectJavaScript(
          `window.updateBoat(${cur.lat},${cur.lon},true,${b.toFixed(2)},${d.toFixed(0)});true;`
        );
      }

      sendWaypoint(ip, wpLat, wpLon).catch(() => {});
    } catch {}
  }, [ip]);

  const handleClearWaypoint = useCallback(() => {
    setWaypoint(null);
    setWpBearing(null);
    setWpDistM(null);
    webViewRef.current?.injectJavaScript('window.clearWaypointMarker();true;');
    const cur = currentGpsRef.current;
    if (cur) {
      webViewRef.current?.injectJavaScript(
        `window.updateBoat(${cur.lat},${cur.lon},true,null,null);true;`
      );
    }
    sendWaypoint(ip, null, null).catch(() => {});
  }, [ip]);

  const handlePickCruise = useCallback((us: number) => {
    sendCruise(ip, { us }).catch(() => {});
    setCruiseModalOpen(false);
  }, [ip]);

  const mode      = data?.mode;
  const failsafe  = mode === 'FAILSAFE';
  const ackNeeded = data?.failsafe_ack === true;
  const captured  = data?.captured === true;
  const cruiseUs  = data?.cruise_us;

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

      {/* Full-width FAILSAFE banner — only when mode says so. */}
      {failsafe && (
        <View style={[styles.failsafeBanner, { paddingTop: insets.top + 8 }]}>
          <Text style={styles.failsafeText}>
            {ackNeeded ? 'FAILSAFE · ACK_REQUIRED — flip SwA UP' : 'FAILSAFE'}
          </Text>
        </View>
      )}

      {/* ── Top control bar ───────────────────────────────────────── */}
      <View
        style={[
          styles.topBarRow,
          { top: (failsafe ? insets.top + 40 : insets.top) + 8 },
        ]}
        pointerEvents="box-none"
      >
        <TouchableOpacity style={styles.chip} onPress={() => navigation.goBack()} activeOpacity={0.7}>
          <Text style={styles.chipBackText}>‹ HELM</Text>
        </TouchableOpacity>

        <View style={styles.chip}>
          <Text style={[styles.chipDot, { color: connected ? Colors.success : Colors.danger }]}>●</Text>
          <Text style={styles.chipMuted}>{connected ? 'CONN' : 'OFFLINE'}</Text>
        </View>

        {mode && (
          <View style={[styles.chip, styles.modeChip, { borderColor: modeColor(mode) }]}>
            <Text style={[styles.chipDot, { color: modeColor(mode) }]}>●</Text>
            <Text style={[styles.modeChipText, { color: modeColor(mode) }]}>{mode}</Text>
          </View>
        )}

        <View style={styles.topBarSpacer} />

        <TouchableOpacity
          style={styles.centerBtn}
          onPress={() => webViewRef.current?.injectJavaScript('window.centerOnBoat();true;')}
          activeOpacity={0.7}
        >
          <Text style={styles.centerBtnText}>CENTER</Text>
        </TouchableOpacity>
      </View>

      {/* ── Action bar (cruise + WP) ──────────────────────────────── */}
      <View
        style={[
          styles.actionBarRow,
          { top: (failsafe ? insets.top + 40 : insets.top) + 50 },
        ]}
        pointerEvents="box-none"
      >
        <TouchableOpacity
          style={styles.cruisePill}
          onPress={() => setCruiseModalOpen(true)}
          activeOpacity={0.7}
        >
          <Text style={styles.cruisePillLabel}>CRUISE</Text>
          <Text style={styles.cruisePillValue}>{cruiseUs ?? '—'}</Text>
          <Text style={styles.cruisePillChevron}>▾</Text>
        </TouchableOpacity>

        {waypoint && (
          <TouchableOpacity style={styles.wpClearBtn} onPress={handleClearWaypoint} activeOpacity={0.7}>
            <Text style={styles.wpClearBtnText}>✕ CLEAR WP</Text>
          </TouchableOpacity>
        )}
      </View>

      {/* Bearing + captured readout. Captured pill replaces the bearing
          line when the boat has reached the waypoint. */}
      {wpBearing !== null && (
        <View
          style={[
            styles.bearingBar,
            { top: (failsafe ? insets.top + 40 : insets.top) + 96 },
          ]}
          pointerEvents="none"
        >
          {captured ? (
            <Text style={styles.capturedText}>✓ CAPTURED</Text>
          ) : (
            <Text style={styles.bearingText}>
              {'→ '}
              {wpBearing.toFixed(1)}°
              {wpDistM !== null
                ? `  ·  ${wpDistM >= 1000 ? (wpDistM / 1000).toFixed(2) + ' km' : Math.round(wpDistM) + ' m'}`
                : ''}
            </Text>
          )}
        </View>
      )}

      <CruiseModal
        visible={cruiseModalOpen}
        currentUs={cruiseUs}
        onPick={handlePickCruise}
        onCancel={() => setCruiseModalOpen(false)}
      />
    </View>
  );
}

const CHIP_BG = 'rgba(10,15,26,0.88)';

const styles = StyleSheet.create({
  screen: { flex: 1, backgroundColor: Colors.background },
  map:    { flex: 1 },

  // Failsafe banner
  failsafeBanner: { position: 'absolute', top: 0, left: 0, right: 0, backgroundColor: Colors.danger, paddingBottom: 8, alignItems: 'center', zIndex: 100 },
  failsafeText:   { color: '#fff', fontSize: 12, fontWeight: '800', letterSpacing: 2, fontFamily: 'monospace' },

  // Top bar
  topBarRow:      { position: 'absolute', left: 12, right: 12, flexDirection: 'row', alignItems: 'center', gap: 6 },
  topBarSpacer:   { flex: 1 },
  chip:           { flexDirection: 'row', alignItems: 'center', backgroundColor: CHIP_BG, paddingHorizontal: 10, paddingVertical: 6, borderRadius: 4 },
  chipBackText:   { color: Colors.accent, fontSize: 11, fontFamily: 'monospace', fontWeight: '800', letterSpacing: 2 },
  chipMuted:      { color: Colors.textSecondary, fontSize: 11, fontFamily: 'monospace', letterSpacing: 1, fontWeight: '700' },
  chipDot:        { fontSize: 9, marginRight: 5 },
  modeChip:       { borderWidth: 1.5, paddingVertical: 5 },
  modeChipText:   { fontSize: 11, fontFamily: 'monospace', fontWeight: '800', letterSpacing: 2 },
  centerBtn:      { backgroundColor: Colors.accent, paddingHorizontal: 14, paddingVertical: 7, borderRadius: 4 },
  centerBtnText:  { color: '#000', fontWeight: '800', fontSize: 11, letterSpacing: 2, fontFamily: 'monospace' },

  // Action bar (cruise + clear-wp)
  actionBarRow:      { position: 'absolute', left: 12, right: 12, flexDirection: 'row', alignItems: 'center', gap: 8 },
  cruisePill:        { flexDirection: 'row', alignItems: 'center', backgroundColor: CHIP_BG, paddingHorizontal: 12, paddingVertical: 8, borderRadius: 4, borderLeftWidth: 2, borderLeftColor: Colors.accent },
  cruisePillLabel:   { color: Colors.textSecondary, fontSize: 10, fontFamily: 'monospace', letterSpacing: 2, fontWeight: '700', marginRight: 8 },
  cruisePillValue:   { color: Colors.accent, fontSize: 13, fontFamily: 'monospace', fontWeight: '800', letterSpacing: 1 },
  cruisePillChevron: { color: Colors.accent, fontSize: 11, marginLeft: 6 },

  wpClearBtn:       { backgroundColor: 'rgba(255,59,48,0.92)', paddingHorizontal: 12, paddingVertical: 8, borderRadius: 4 },
  wpClearBtnText:   { color: '#fff', fontSize: 11, fontFamily: 'monospace', fontWeight: '800', letterSpacing: 1 },

  // Bearing / captured readouts
  bearingBar:    { position: 'absolute', left: 16, right: 16, alignItems: 'center' },
  bearingText:   { backgroundColor: 'rgba(0,230,118,0.15)', color: '#00e676', fontFamily: 'monospace', fontSize: 14, fontWeight: '800', paddingHorizontal: 14, paddingVertical: 5, borderRadius: 4, overflow: 'hidden', letterSpacing: 1 },
  capturedText:  { backgroundColor: 'rgba(52, 199, 89, 0.25)', color: '#34c759', fontFamily: 'monospace', fontSize: 14, fontWeight: '800', letterSpacing: 3, paddingHorizontal: 14, paddingVertical: 5, borderRadius: 4, overflow: 'hidden' },
});

