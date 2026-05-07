// MapScreen.tsx
// Real-time boat position on a Leaflet.js map rendered inside a WebView.
// Works with Expo Go — no native dev build required.
//
// NOTE: OpenStreetMap tiles need internet access. When the phone is connected
// to the boat's WiFi AP (no internet), iOS WiFi Assist routes tile requests
// over cellular automatically. If tiles don't load, the marker + track trail
// still update — coordinates show in the HUD overlay at the bottom.

import React, { useRef, useEffect } from 'react';
import { View, TouchableOpacity, Text, StyleSheet } from 'react-native';
import { WebView } from 'react-native-webview';
import { NativeStackScreenProps } from '@react-navigation/native-stack';
import { RootStackParamList } from '../../App';
import { Colors } from '../constants';
import { useTelemetry } from '../hooks/useTelemetry';
import EmergencyStop from '../components/EmergencyStop';

type Props = NativeStackScreenProps<RootStackParamList, 'Map'>;

// Self-contained HTML — Leaflet loaded from CDN.
// updateBoat(lat, lon, hasFix) is called from React Native via injectJavaScript.
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
      position:absolute;bottom:90px;left:12px;right:12px;
      background:rgba(10,15,26,0.88);color:#6b8fa8;
      padding:7px 14px;border-radius:8px;
      font:13px/1.5 monospace;z-index:1000;
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

    var icon = L.divIcon({
      html:'<div class="boat">⛵</div>',
      iconSize:[28,28],iconAnchor:[14,14],className:''
    });
    var marker  = null;
    var trail   = L.polyline([],{color:'#00bfff',weight:2.5,opacity:0.8}).addTo(map);
    var pts     = [];
    var locked  = false;   // true = map pans to follow the boat

    window.updateBoat = function(lat, lon, hasFix) {
      var hud = document.getElementById('hud');
      if (!hasFix) { hud.textContent = 'No GPS fix'; return; }
      var ll = [lat, lon];
      if (!marker) {
        marker = L.marker(ll,{icon:icon}).addTo(map);
        map.setView(ll, 16);   // first fix — zoom in once
      } else {
        marker.setLatLng(ll);
        if (locked) map.panTo(ll,{animate:true,duration:0.5});
      }
      pts.push(ll);
      if (pts.length > 800) pts.shift();
      trail.setLatLngs(pts);
      hud.textContent = lat.toFixed(6) + ',  ' + lon.toFixed(6);
    };

    // Called by CENTER button in React Native
    window.centerOnBoat = function() {
      if (marker) { map.setView(marker.getLatLng(), map.getZoom()); locked = true; }
    };

    // User dragging the map disengages follow-lock
    map.on('dragstart', function() { locked = false; });
  </script>
</body>
</html>`;

export default function MapScreen({ route }: Props) {
  const { ip } = route.params;
  const { data, connected } = useTelemetry();
  const webViewRef = useRef<WebView>(null);

  // Push GPS updates into the WebView whenever lat/lon or fix status changes.
  useEffect(() => {
    if (!webViewRef.current || !data) return;
    const lat    = parseFloat(data.lat ?? '');
    const lon    = parseFloat(data.lon ?? '');
    const hasFix = data.gps_fix === true && !isNaN(lat) && !isNaN(lon);
    webViewRef.current.injectJavaScript(
      `window.updateBoat(${hasFix ? lat : 0},${hasFix ? lon : 0},${hasFix});true;`
    );
  }, [data?.gps_fix, data?.lat, data?.lon]);

  return (
    <View style={styles.container}>
      <WebView
        ref={webViewRef}
        source={{ html: MAP_HTML }}
        style={styles.map}
        originWhitelist={['*']}
        javaScriptEnabled
        domStorageEnabled
      />

      {/* Connection dot + CENTER button floating above the map */}
      <View style={styles.topBar}>
        <View style={styles.connRow}>
          <Text style={[styles.dot, { color: connected ? Colors.success : Colors.danger }]}>●</Text>
          <Text style={styles.connText}>{connected ? 'CONNECTED' : 'OFFLINE'}</Text>
        </View>
        <TouchableOpacity
          style={styles.centerBtn}
          onPress={() => webViewRef.current?.injectJavaScript('window.centerOnBoat();true;')}
        >
          <Text style={styles.centerBtnText}>CENTER</Text>
        </TouchableOpacity>
      </View>

      <EmergencyStop ip={ip} />
    </View>
  );
}

const styles = StyleSheet.create({
  container:     { flex: 1, backgroundColor: Colors.background },
  map:           { flex: 1 },
  topBar:        {
    position: 'absolute', top: 16, left: 16, right: 16,
    flexDirection: 'row', justifyContent: 'space-between', alignItems: 'center',
    pointerEvents: 'box-none',
  },
  connRow:       { flexDirection: 'row', alignItems: 'center', backgroundColor: 'rgba(10,15,26,0.8)', paddingHorizontal: 10, paddingVertical: 5, borderRadius: 12 },
  dot:           { fontSize: 10, marginRight: 5 },
  connText:      { color: Colors.textSecondary, fontSize: 11, fontFamily: 'monospace' },
  centerBtn:     { backgroundColor: Colors.accent, paddingHorizontal: 14, paddingVertical: 7, borderRadius: 8 },
  centerBtnText: { color: '#000', fontWeight: 'bold', fontSize: 12, letterSpacing: 1 },
});
