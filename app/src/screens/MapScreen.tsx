import React, { useRef, useEffect, useState, useCallback } from 'react';
import { View, TouchableOpacity, Text, StyleSheet } from 'react-native';
import { WebView } from 'react-native-webview';
import { useSafeAreaInsets } from 'react-native-safe-area-context';
import { NativeStackScreenProps } from '@react-navigation/native-stack';
import * as Haptics from 'expo-haptics';
import { RootStackParamList } from '../../App';
import { Colors } from '../constants';
import { useTelemetry } from '../hooks/useTelemetry';
import { setWaypoint as sendWaypoint, setCruise as sendCruise } from '../services/esp32Service';
import { CruiseModal } from '../components/CruiseModal';

type Props = NativeStackScreenProps<RootStackParamList, 'Map'>;

// ── Haversine bearing and distance (used for the in-map HUD overlay) ──────────
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
// All map-side rendering lives here: boat marker, waypoint reticle, planned
// path polyline, and a HUD that has three states:
//   hidden:  fix is good and no waypoint → boat marker speaks for itself
//   minimal: no fix → small yellow ⚠ NO GPS FIX line (no card chrome)
//   card:    waypoint tracking or captured → full bordered card
// HUD starts hidden so a remount that loads before the first telemetry
// frame doesn't flash "NO GPS FIX" before we actually know GPS state.
//
// Color split: the WP marker and planned path use bright cyan (the
// app accent — high visibility in sun, signals "active target"). The
// boat trail uses a muted steel blue so the history doesn't compete
// with the live target.
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
      position:absolute;
      bottom:80px;
      left:50%;
      transform:translateX(-50%);
      font-family:monospace;
      text-align:center;
      z-index:1000;
      pointer-events:none;
    }
    #hud.hud-hidden  { display:none; }
    #hud.hud-minimal { padding:5px 12px; }
    #hud.hud-minimal #hud-primary{
      font-size:11px; font-weight:700; letter-spacing:1.5px;
    }
    #hud.hud-minimal #hud-secondary{ display:none; }
    #hud.hud-card{
      background:rgba(10,15,26,0.92);
      padding:14px 22px;
      border-radius:6px;
      border:1px solid rgba(0,191,255,0.25);
      min-width:220px;
      max-width:78%;
    }
    #hud.hud-card #hud-primary{
      font-size:22px;
      font-weight:800;
      letter-spacing:1px;
      line-height:1.15;
      white-space:nowrap;
    }
    #hud.hud-card #hud-secondary{
      font-size:10px;
      color:#6b8fa8;
      letter-spacing:1.5px;
      margin-top:8px;
      font-weight:600;
    }

    .boat{font-size:22px;line-height:1}

    /* Waypoint reticle — bright cyan, the active target color. */
    .wp-icon{background:transparent !important;border:none !important}
    .wp-target{position:relative;width:30px;height:30px}
    .wp-ring{
      position:absolute;top:1px;left:1px;
      width:28px;height:28px;
      border:2.5px solid #00bfff;border-radius:50%;
      background:rgba(0,191,255,0.2);
      box-shadow:0 0 8px rgba(0,191,255,0.7);
    }
    .wp-dot{
      position:absolute;top:12px;left:12px;
      width:6px;height:6px;
      background:#00bfff;border-radius:50%;
      box-shadow:0 0 4px rgba(0,191,255,0.9);
    }
  </style>
</head>
<body>
  <div id="map"></div>
  <div id="hud" class="hud-hidden">
    <div id="hud-primary"></div>
    <div id="hud-secondary"></div>
  </div>
  <script>
    // zoomControl off — pinch-zoom still works; the in-map buttons
    // overlapped the iOS status bar at the top-left.
    var map = L.map('map', { zoomControl:false }).setView([37.8,-122.4], 13);
    L.tileLayer('https://{s}.tile.openstreetmap.org/{z}/{x}/{y}.png', {
      attribution:'© OpenStreetMap contributors', maxZoom:19
    }).addTo(map);

    var boatIcon = L.divIcon({
      html:'<div class="boat">⛵</div>',
      iconSize:[28,28], iconAnchor:[14,14], className:''
    });
    var wpIconDef = L.divIcon({
      html:'<div class="wp-target"><div class="wp-ring"></div><div class="wp-dot"></div></div>',
      iconSize:[30,30], iconAnchor:[15,15], className:'wp-icon'
    });

    var marker   = null;
    var wpMarker = null;
    var pathLine = null;
    // Trail = where we've been. Muted steel blue so it doesn't compete
    // with the bright-cyan waypoint and path-line.
    var trail    = L.polyline([], { color:'#3a6db8', weight:2.5, opacity:0.75 }).addTo(map);
    var pts      = [];
    var locked   = false;

    function setHud(mode, primary, secondary, color) {
      var card = document.getElementById('hud');
      card.className = 'hud-' + mode;
      if (mode === 'hidden') return;
      var p = document.getElementById('hud-primary');
      p.textContent = primary || '';
      p.style.color = color || '#e8f4fd';
      document.getElementById('hud-secondary').textContent = secondary || '';
    }

    // Dashed cyan planned-path line between boat and waypoint.
    function updatePathLine() {
      if (marker && wpMarker) {
        var coords = [marker.getLatLng(), wpMarker.getLatLng()];
        if (pathLine) {
          pathLine.setLatLngs(coords);
        } else {
          pathLine = L.polyline(coords, {
            color:'#00bfff', weight:2.5, dashArray:'8, 6', opacity:0.9
          }).addTo(map);
        }
      } else if (pathLine) {
        map.removeLayer(pathLine);
        pathLine = null;
      }
    }

    window.updateBoat = function(lat, lon, hasFix, bearing, distM, captured) {
      if (!hasFix) {
        setHud('minimal', '⚠ NO GPS FIX', '', '#ffcc00');
        return;
      }
      var ll = [lat, lon];
      if (!marker) {
        marker = L.marker(ll, { icon:boatIcon }).addTo(map);
        map.setView(ll, 18);
      } else {
        marker.setLatLng(ll);
        if (locked) map.panTo(ll, { animate:true, duration:0.5 });
      }
      pts.push(ll);
      if (pts.length > 800) pts.shift();
      trail.setLatLngs(pts);
      updatePathLine();

      var llStr = lat.toFixed(6) + ', ' + lon.toFixed(6);

      if (captured) {
        setHud('card', '✓ CAPTURED', llStr, '#34c759');
      } else if (bearing != null && distM != null && !isNaN(bearing)) {
        var d = distM >= 1000
          ? (distM / 1000).toFixed(2) + ' km'
          : Math.round(distM) + ' m';
        setHud('card', '→ ' + bearing.toFixed(0) + '°  ·  ' + d, llStr, '#34c759');
      } else {
        // Fix but no waypoint — boat marker on the map speaks for itself.
        setHud('hidden');
      }
    };

    window.centerOnBoat = function() {
      if (marker) {
        map.setView(marker.getLatLng(), map.getZoom());
        locked = true;
      }
    };

    window.setWaypointMarker = function(lat, lon) {
      if (wpMarker) {
        wpMarker.setLatLng([lat, lon]);
      } else {
        wpMarker = L.marker([lat, lon], { icon:wpIconDef }).addTo(map);
      }
      updatePathLine();
    };

    window.clearWaypointMarker = function() {
      if (wpMarker) { map.removeLayer(wpMarker); wpMarker = null; }
      updatePathLine();
    };

    map.on('dragstart', function() { locked = false; });

    map.on('click', function(e) {
      window.ReactNativeWebView.postMessage(JSON.stringify({
        type:'waypoint',
        lat:e.latlng.lat,
        lon:e.latlng.lng
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
  const { data } = useTelemetry();
  const webViewRef    = useRef<WebView>(null);
  const insets        = useSafeAreaInsets();
  const currentGpsRef = useRef<{ lat: number; lon: number } | null>(null);

  const [waypoint, setWaypoint] = useState<{ lat: number; lon: number } | null>(null);
  const [cruiseModalOpen, setCruiseModalOpen] = useState(false);
  const [webViewReady, setWebViewReady] = useState(false);

  // Rehydrate from telemetry ONCE per mount. Local state is authoritative
  // after that — otherwise stale telemetry (the POST /waypoint clear
  // hasn't propagated to the next /telemetry frame yet) races the user's
  // CLEAR tap and visually undoes it. The ref also flips to true the
  // moment the user takes any local action, so a clear that arrives
  // before any telemetry-driven hydrate also blocks future hydrates.
  const hydratedRef = useRef(false);

  useEffect(() => {
    if (hydratedRef.current) return;
    if (!data?.wp_set) return;
    if (data.wp_lat == null || data.wp_lon == null) return;
    const lat = parseFloat(data.wp_lat);
    const lon = parseFloat(data.wp_lon);
    if (isNaN(lat) || isNaN(lon)) return;
    hydratedRef.current = true;
    setWaypoint({ lat, lon });
  }, [data?.wp_set, data?.wp_lat, data?.wp_lon]);

  useEffect(() => {
    if (waypoint != null) hydratedRef.current = true;
  }, [waypoint]);

  // Reflect the local waypoint state into the WebView.
  useEffect(() => {
    if (!webViewReady || !webViewRef.current) return;
    if (waypoint) {
      webViewRef.current.injectJavaScript(
        `window.setWaypointMarker(${waypoint.lat},${waypoint.lon});true;`
      );
    } else {
      webViewRef.current.injectJavaScript('window.clearWaypointMarker();true;');
    }
  }, [waypoint, webViewReady]);

  // Inject boat position + HUD state whenever telemetry changes (or after
  // the WebView finishes loading, so a remount shows the right state
  // immediately rather than flashing the default view + NO FIX).
  useEffect(() => {
    if (!webViewReady || !webViewRef.current || !data) return;
    const lat    = parseFloat(data.lat ?? '');
    const lon    = parseFloat(data.lon ?? '');
    const hasFix = data.gps_fix === true && !isNaN(lat) && !isNaN(lon);
    const cap    = data.captured === true;

    if (hasFix) currentGpsRef.current = { lat, lon };

    let bearing: number | null = null;
    let dist:    number | null = null;
    if (hasFix && waypoint) {
      bearing = bearingTo(lat, lon, waypoint.lat, waypoint.lon);
      dist    = distanceTo(lat, lon, waypoint.lat, waypoint.lon);
    }

    webViewRef.current.injectJavaScript(
      `window.updateBoat(${hasFix ? lat : 0},${hasFix ? lon : 0},${hasFix},` +
      `${bearing ?? 'null'},${dist ?? 'null'},${cap});true;`
    );
  }, [data?.gps_fix, data?.lat, data?.lon, data?.captured, waypoint, webViewReady]);

  // Handle tap-on-map messages from the WebView.
  const handleMapMessage = useCallback((msgData: string) => {
    try {
      const msg = JSON.parse(msgData);
      if (msg.type !== 'waypoint') return;
      const wpLat: number = msg.lat;
      const wpLon: number = msg.lon;
      Haptics.impactAsync(Haptics.ImpactFeedbackStyle.Medium);
      setWaypoint({ lat: wpLat, lon: wpLon });
      sendWaypoint(ip, wpLat, wpLon).catch(() => {});
    } catch {}
  }, [ip]);

  const handleClearWaypoint = useCallback(() => {
    Haptics.impactAsync(Haptics.ImpactFeedbackStyle.Medium);
    setWaypoint(null);
    sendWaypoint(ip, null, null).catch(() => {});
  }, [ip]);

  const handlePickCruise = useCallback((us: number) => {
    sendCruise(ip, { us }).catch(() => {});
    setCruiseModalOpen(false);
  }, [ip]);

  const mode      = data?.mode;
  const failsafe  = mode === 'FAILSAFE';
  const ackNeeded = data?.failsafe_ack === true;
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
        onLoadEnd={() => setWebViewReady(true)}
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

      {/* ── Top bar: MODE only (back button is at the bottom) ─────── */}
      <View
        style={[
          styles.topBarRow,
          { top: (failsafe ? insets.top + 40 : insets.top) + 8 },
        ]}
        pointerEvents="box-none"
      >
        <View style={styles.topBarSpacer} />

        {mode && (
          <View style={[styles.chip, styles.modeChip, { borderColor: modeColor(mode) }]}>
            <Text style={[styles.chipDot, { color: modeColor(mode) }]}>●</Text>
            <Text style={[styles.modeChipText, { color: modeColor(mode) }]}>{mode}</Text>
          </View>
        )}
      </View>

      {/* ── Action bar (cruise + clear WP) ────────────────────────── */}
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

      {/* ── Back-to-Helm chip (bottom left) ───────────────────────── */}
      <TouchableOpacity
        style={[styles.backFab, { bottom: insets.bottom + 24 }]}
        onPress={() => navigation.goBack()}
        activeOpacity={0.7}
      >
        <Text style={styles.backFabText}>‹ HELM</Text>
      </TouchableOpacity>

      {/* ── Center-on-boat FAB (bottom right) ─────────────────────── */}
      <TouchableOpacity
        style={[styles.centerFab, { bottom: insets.bottom + 24 }]}
        onPress={() => webViewRef.current?.injectJavaScript('window.centerOnBoat();true;')}
        activeOpacity={0.7}
      >
        <Text style={styles.centerFabIcon}>⊙</Text>
      </TouchableOpacity>

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
  chipDot:        { fontSize: 9, marginRight: 5 },
  modeChip:       { borderWidth: 1.5, paddingVertical: 5 },
  modeChipText:   { fontSize: 11, fontFamily: 'monospace', fontWeight: '800', letterSpacing: 2 },

  // Action bar (cruise + clear-wp)
  actionBarRow:      { position: 'absolute', left: 12, right: 12, flexDirection: 'row', alignItems: 'center', gap: 8 },
  cruisePill:        { flexDirection: 'row', alignItems: 'center', backgroundColor: CHIP_BG, paddingHorizontal: 12, paddingVertical: 8, borderRadius: 4, borderLeftWidth: 2, borderLeftColor: Colors.accent },
  cruisePillLabel:   { color: Colors.textSecondary, fontSize: 10, fontFamily: 'monospace', letterSpacing: 2, fontWeight: '700', marginRight: 8 },
  cruisePillValue:   { color: Colors.accent, fontSize: 13, fontFamily: 'monospace', fontWeight: '800', letterSpacing: 1 },
  cruisePillChevron: { color: Colors.accent, fontSize: 11, marginLeft: 6 },

  wpClearBtn:     { backgroundColor: 'rgba(255,59,48,0.92)', paddingHorizontal: 12, paddingVertical: 8, borderRadius: 4 },
  wpClearBtnText: { color: '#fff', fontSize: 11, fontFamily: 'monospace', fontWeight: '800', letterSpacing: 1 },

  // Center FAB (bottom-right)
  centerFab: {
    position: 'absolute',
    right: 16,
    width: 52, height: 52,
    borderRadius: 26,
    backgroundColor: 'rgba(10,15,26,0.92)',
    borderWidth: 1.5, borderColor: Colors.accent,
    alignItems: 'center', justifyContent: 'center',
    shadowColor: '#000', shadowOpacity: 0.4, shadowOffset: { width: 0, height: 2 }, shadowRadius: 4,
    elevation: 4,
  },
  centerFabIcon: { color: Colors.accent, fontSize: 24, fontWeight: '800', lineHeight: 24 },

  // Back-to-Helm chip (bottom-left). Larger tap target than the original
  // top-right chip so it's reachable one-handed without resetting the map.
  backFab: {
    position: 'absolute',
    left: 16,
    height: 52,
    paddingHorizontal: 18,
    borderRadius: 26,
    backgroundColor: 'rgba(10,15,26,0.92)',
    borderWidth: 1.5, borderColor: Colors.accent,
    alignItems: 'center', justifyContent: 'center',
    shadowColor: '#000', shadowOpacity: 0.4, shadowOffset: { width: 0, height: 2 }, shadowRadius: 4,
    elevation: 4,
  },
  backFabText: { color: Colors.accent, fontSize: 13, fontFamily: 'monospace', fontWeight: '800', letterSpacing: 2 },
});
