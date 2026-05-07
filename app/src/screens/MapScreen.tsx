import React, { useRef, useEffect } from 'react';
import { View, TouchableOpacity, Text, StyleSheet } from 'react-native';
import { WebView } from 'react-native-webview';
import { useSafeAreaInsets } from 'react-native-safe-area-context';
import { NativeStackScreenProps } from '@react-navigation/native-stack';
import { RootStackParamList } from '../../App';
import { Colors } from '../constants';
import { useTelemetry } from '../hooks/useTelemetry';

type Props = NativeStackScreenProps<RootStackParamList, 'Map'>;

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
    var locked  = false;

    window.updateBoat = function(lat, lon, hasFix) {
      var hud = document.getElementById('hud');
      if (!hasFix) { hud.textContent = 'No GPS fix'; return; }
      var ll = [lat, lon];
      if (!marker) {
        marker = L.marker(ll,{icon:icon}).addTo(map);
        map.setView(ll, 16);
      } else {
        marker.setLatLng(ll);
        if (locked) map.panTo(ll,{animate:true,duration:0.5});
      }
      pts.push(ll);
      if (pts.length > 800) pts.shift();
      trail.setLatLngs(pts);
      hud.textContent = lat.toFixed(6) + ',  ' + lon.toFixed(6);
    };

    window.centerOnBoat = function() {
      if (marker) { map.setView(marker.getLatLng(), map.getZoom()); locked = true; }
    };

    map.on('dragstart', function() { locked = false; });
  </script>
</body>
</html>`;

export default function MapScreen({ route, navigation }: Props) {
  const { ip } = route.params;
  const { data, connected } = useTelemetry();
  const webViewRef = useRef<WebView>(null);
  const insets = useSafeAreaInsets();

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
    <View style={styles.screen}>
      <WebView
        ref={webViewRef}
        source={{ html: MAP_HTML }}
        style={styles.map}
        originWhitelist={['*']}
        javaScriptEnabled
        domStorageEnabled
      />

      <View style={[styles.topBar, { top: insets.top + 8 }]} pointerEvents="box-none">
        <TouchableOpacity style={styles.backBtn} onPress={() => navigation.goBack()}>
          <Text style={styles.backBtnText}>‹ BACK</Text>
        </TouchableOpacity>
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
    </View>
  );
}

const styles = StyleSheet.create({
  screen:        { flex: 1, backgroundColor: Colors.background },
  map:           { flex: 1 },
  topBar:        {
    position: 'absolute', left: 16, right: 16,
    flexDirection: 'row', justifyContent: 'space-between', alignItems: 'center',
  },
  backBtn:       { backgroundColor: 'rgba(10,15,26,0.85)', paddingHorizontal: 10, paddingVertical: 5, borderRadius: 12 },
  backBtnText:   { color: Colors.textSecondary, fontSize: 11, fontFamily: 'monospace' },
  connRow:       { flexDirection: 'row', alignItems: 'center', backgroundColor: 'rgba(10,15,26,0.85)', paddingHorizontal: 10, paddingVertical: 5, borderRadius: 12 },
  dot:           { fontSize: 10, marginRight: 5 },
  connText:      { color: Colors.textSecondary, fontSize: 11, fontFamily: 'monospace' },
  centerBtn:     { backgroundColor: Colors.accent, paddingHorizontal: 14, paddingVertical: 7, borderRadius: 8 },
  centerBtnText: { color: '#000', fontWeight: 'bold', fontSize: 12, letterSpacing: 1 },
});
