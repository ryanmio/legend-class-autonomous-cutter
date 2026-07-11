// FlightDetailScreen — per-flight summary, GPS-track map, share CSV.
// Stats come from the FlightMeta in the index (cheap). The polyline
// requires parsing the flight's CSV, which we do once on mount.

import React, { useCallback, useEffect, useRef, useState } from 'react';
import {
  View, Text, StyleSheet, TouchableOpacity, ActivityIndicator, Alert, Share,
} from 'react-native';
import { WebView } from 'react-native-webview';
import { NativeStackScreenProps } from '@react-navigation/native-stack';
import * as Sharing from 'expo-sharing';
import { RootStackParamList } from '../../App';
import { Colors } from '../constants';
import Screen from '../components/Screen';
import {
  FlightMeta, listFlights, deleteFlight,
  loadFlightCSV, loadFlightRows, extractTrack, getFlightFileUri, TrackPoint,
} from '../services/telemetryLogger';

type Props = NativeStackScreenProps<RootStackParamList, 'FlightDetail'>;

// ── Format helpers ────────────────────────────────────────────────────────

function fmtTimestamp(ms: number): string {
  const d = new Date(ms);
  const pad = (n: number) => String(n).padStart(2, '0');
  return `${d.getFullYear()}-${pad(d.getMonth() + 1)}-${pad(d.getDate())} ${pad(d.getHours())}:${pad(d.getMinutes())}`;
}

function fmtDuration(ms: number): string {
  const totalSec = Math.max(0, Math.round(ms / 1000));
  const h = Math.floor(totalSec / 3600);
  const m = Math.floor((totalSec % 3600) / 60);
  const s = totalSec % 60;
  if (h > 0) return `${h}h ${pad2(m)}m`;
  if (m > 0) return `${m}m ${pad2(s)}s`;
  return `${s}s`;
}
function pad2(n: number) { return String(n).padStart(2, '0'); }

function fmtDistance(m: number | undefined): string {
  if (m == null) return '—';
  if (m >= 1000) return `${(m / 1000).toFixed(2)} km`;
  return `${Math.round(m)} m`;
}

function fmtSpeed(kts: number | undefined): string {
  if (kts == null || kts <= 0) return '—';
  return `${kts.toFixed(1)} kts`;
}

function fmtDepth(m: number | undefined): string {
  if (m == null || m <= 0) return '—';
  return `${m.toFixed(2)} m`;
}

function fmtBattery(start: number | undefined, end: number | undefined): string {
  if (start == null && end == null) return '—';
  if (start == null) return `→ ${end!.toFixed(2)} V`;
  if (end   == null) return `${start.toFixed(2)} V`;
  return `${start.toFixed(2)} → ${end.toFixed(2)} V`;
}

function fmtModeMix(meta: FlightMeta): string {
  const a = meta.autoMs ?? 0;
  const m = meta.manualMs ?? 0;
  const f = meta.failsafeMs ?? 0;
  const total = a + m + f;
  if (total === 0) return '—';
  const parts: string[] = [];
  if (a > 0) parts.push(`AUTO ${fmtDuration(a)}`);
  if (m > 0) parts.push(`MAN ${fmtDuration(m)}`);
  if (f > 0) parts.push(`FS ${fmtDuration(f)}`);
  return parts.join(' / ');
}

// ── Map HTML (Leaflet polyline of the flight track) ───────────────────────

const TRACK_MAP_HTML = `<!DOCTYPE html>
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
    .endpoint{background:transparent !important;border:none !important}
    .ep-dot{
      width:14px;height:14px;border-radius:50%;
      border:2px solid #0a0f1a;
    }
    .ep-start{background:#34c759;box-shadow:0 0 6px rgba(52,199,89,0.8)}
    .ep-end  {background:#ff3b30;box-shadow:0 0 6px rgba(255,59,48,0.8)}

    #layer-toggle{
      position:absolute;
      top:10px;
      right:10px;
      background:rgba(10,15,26,0.92);
      border:1.5px solid #00bfff;
      border-radius:4px;
      color:#00bfff;
      font-family:monospace;
      font-weight:800;
      font-size:11px;
      letter-spacing:2px;
      padding:6px 10px;
      z-index:1000;
      cursor:pointer;
      user-select:none;
    }
  </style>
</head>
<body>
  <div id="map"></div>
  <div id="layer-toggle">MAP</div>
  <script>
    var map = L.map('map', { zoomControl:false, attributionControl:false })
      .setView([37.8, -122.4], 13);

    var satLayer = L.tileLayer(
      'https://server.arcgisonline.com/ArcGIS/rest/services/World_Imagery/MapServer/tile/{z}/{y}/{x}',
      { maxZoom: 19 }
    );
    var osmLayer = L.tileLayer('https://{s}.tile.openstreetmap.org/{z}/{x}/{y}.png', {
      maxZoom: 19
    });
    var currentLayer = satLayer;
    currentLayer.addTo(map);

    document.getElementById('layer-toggle').addEventListener('click', function() {
      if (currentLayer === satLayer) {
        map.removeLayer(satLayer); osmLayer.addTo(map); currentLayer = osmLayer;
        this.textContent = 'SAT';
      } else {
        map.removeLayer(osmLayer); satLayer.addTo(map); currentLayer = satLayer;
        this.textContent = 'MAP';
      }
    });

    window.drawTrack = function(points) {
      if (!points || points.length === 0) return;
      var latlngs = points.map(function(p) { return [p[0], p[1]]; });
      var line = L.polyline(latlngs, {
        color:'#00bfff', weight:3, opacity:0.95
      }).addTo(map);

      var startIcon = L.divIcon({
        html:'<div class="ep-dot ep-start"></div>',
        iconSize:[14,14], iconAnchor:[7,7], className:'endpoint'
      });
      var endIcon = L.divIcon({
        html:'<div class="ep-dot ep-end"></div>',
        iconSize:[14,14], iconAnchor:[7,7], className:'endpoint'
      });
      L.marker(latlngs[0],                { icon:startIcon }).addTo(map);
      L.marker(latlngs[latlngs.length-1], { icon:endIcon   }).addTo(map);

      // Fit, then pad out a touch so endpoint markers aren't on the edge.
      map.fitBounds(line.getBounds(), { padding: [28, 28] });
      // Cap zoom so a tiny pool doesn't render as a single pixel.
      if (map.getZoom() > 19) map.setZoom(19);
    };
  </script>
</body>
</html>`;

// ── Screen ────────────────────────────────────────────────────────────────

export default function FlightDetailScreen({ route, navigation }: Props) {
  const { id } = route.params;
  const [meta, setMeta]   = useState<FlightMeta | null>(null);
  const [track, setTrack] = useState<TrackPoint[] | null>(null);
  const [loading, setLoading] = useState(true);
  const [webViewReady, setWebViewReady] = useState(false);
  const webViewRef = useRef<WebView>(null);

  // Load meta + track on mount.
  useEffect(() => {
    let cancelled = false;
    (async () => {
      const all = await listFlights();
      const m = all.find((x) => x.id === id) ?? null;
      if (cancelled) return;
      setMeta(m);

      if (m && (m.gpsRowCount ?? 0) > 0) {
        const rows = await loadFlightRows(id);
        if (cancelled) return;
        setTrack(extractTrack(rows));
      } else {
        setTrack([]);
      }
      setLoading(false);
    })();
    return () => { cancelled = true; };
  }, [id]);

  // Inject the polyline once both the WebView and the track are ready.
  useEffect(() => {
    if (!webViewReady || !webViewRef.current || track == null) return;
    if (track.length === 0) return;
    const payload = JSON.stringify(track.map((p) => [p.lat, p.lon]));
    webViewRef.current.injectJavaScript(`window.drawTrack(${payload});true;`);
  }, [webViewReady, track]);

  const onShare = useCallback(async () => {
    try {
      const uri = getFlightFileUri(id);
      const canShareFile = await Sharing.isAvailableAsync();
      if (canShareFile) {
        await Sharing.shareAsync(uri, {
          mimeType: 'text/csv',
          UTI: 'public.comma-separated-values-text',
          dialogTitle: `Flight ${id}`,
        });
      } else {
        // Web / unsupported platform fallback — share as message body.
        const csv = await loadFlightCSV(id);
        if (csv) await Share.share({ title: `${id}.csv`, message: csv });
      }
    } catch (e) {
      Alert.alert('Share failed', String(e));
    }
  }, [id]);

  const onDelete = useCallback(() => {
    if (!meta) return;
    Alert.alert(
      'Delete flight?',
      `${fmtTimestamp(meta.startTs)} · ${meta.rowCount} rows. This cannot be undone.`,
      [
        { text: 'Cancel', style: 'cancel' },
        {
          text: 'Delete',
          style: 'destructive',
          onPress: async () => {
            await deleteFlight(meta.id);
            navigation.goBack();
          },
        },
      ],
    );
  }, [meta, navigation]);

  return (
    <Screen>
      <View style={styles.screen}>
        {/* ── Top bar ─────────────────────────────────────── */}
        <View style={styles.topBar}>
          <TouchableOpacity onPress={() => navigation.goBack()} style={styles.backBtn}>
            <Text style={styles.backBtnText}>‹ BACK</Text>
          </TouchableOpacity>
          <View style={styles.titleBlock}>
            <Text style={styles.pageTitle}>FLIGHT</Text>
            {meta && <Text style={styles.subtitle}>{fmtTimestamp(meta.startTs)}</Text>}
          </View>
          <TouchableOpacity onPress={onDelete} style={styles.deleteBtn} hitSlop={8} disabled={!meta}>
            <Text style={styles.deleteBtnText}>×</Text>
          </TouchableOpacity>
        </View>

        {loading || !meta ? (
          <View style={styles.loadingBlock}>
            <ActivityIndicator color={Colors.accent} />
          </View>
        ) : (
          <>
            {/* ── Stats grid ────────────────────────────────── */}
            <View style={styles.statsCard}>
              <View style={styles.statsRow}>
                <Stat label="DURATION"  value={fmtDuration(meta.endTs - meta.startTs)} />
                <Stat label="DISTANCE"  value={fmtDistance(meta.distanceM)} />
                <Stat label="MAX SPEED" value={fmtSpeed(meta.maxSpeedKts)} />
              </View>
              <View style={styles.statsRow}>
                <Stat label="MAX DEPTH" value={fmtDepth(meta.maxDepthM)} />
                <Stat label="BATTERY"   value={fmtBattery(meta.battStartV, meta.battEndV)} flex={2} />
              </View>
              <View style={styles.modeMixRow}>
                <Text style={styles.modeMixLabel}>MODE</Text>
                <Text style={styles.modeMixValue}>{fmtModeMix(meta)}</Text>
              </View>
              {meta.capturedWaypoint && (
                <View style={styles.captureBadge}>
                  <Text style={styles.captureBadgeText}>✓ WAYPOINT CAPTURED</Text>
                </View>
              )}
            </View>

            {/* ── Map ───────────────────────────────────────── */}
            <View style={styles.mapBlock}>
              {track && track.length > 0 ? (
                <WebView
                  ref={webViewRef}
                  source={{ html: TRACK_MAP_HTML }}
                  style={styles.map}
                  originWhitelist={['*']}
                  javaScriptEnabled
                  domStorageEnabled
                  onLoadEnd={() => setWebViewReady(true)}
                />
              ) : (
                <View style={styles.mapPlaceholder}>
                  <Text style={styles.mapPlaceholderTitle}>NO GPS DATA</Text>
                  <Text style={styles.mapPlaceholderBody}>
                    This flight has no rows with a GPS fix.
                    {'\n'}Telemetry-only data is still in the CSV.
                  </Text>
                </View>
              )}
            </View>

            {/* ── Share button ──────────────────────────────── */}
            <TouchableOpacity style={styles.shareBtn} onPress={onShare} activeOpacity={0.7}>
              <Text style={styles.shareBtnText}>SHARE CSV</Text>
            </TouchableOpacity>
            <Text style={styles.footnote}>
              {meta.rowCount} row{meta.rowCount === 1 ? '' : 's'}
              {meta.gpsRowCount != null && ` · ${meta.gpsRowCount} with GPS`}
            </Text>
          </>
        )}
      </View>
    </Screen>
  );
}

function Stat({ label, value, flex = 1 }: { label: string; value: string; flex?: number }) {
  return (
    <View style={[styles.stat, { flex }]}>
      <Text style={styles.statLabel}>{label}</Text>
      <Text style={styles.statValue}>{value}</Text>
    </View>
  );
}

const styles = StyleSheet.create({
  screen: { flex: 1, paddingHorizontal: 16, paddingTop: 12 },

  topBar:        { flexDirection: 'row', alignItems: 'center', marginBottom: 12 },
  backBtn:       { paddingVertical: 6, paddingRight: 12 },
  backBtnText:   { color: Colors.accent, fontSize: 12, fontFamily: 'monospace', letterSpacing: 2, fontWeight: '700' },
  titleBlock:    { flex: 1, alignItems: 'center' },
  pageTitle:     { color: Colors.textPrimary, fontSize: 14, fontFamily: 'monospace', letterSpacing: 4, fontWeight: '800' },
  subtitle:      { color: Colors.textSecondary, fontSize: 10, fontFamily: 'monospace', letterSpacing: 2, marginTop: 2 },
  deleteBtn:     { paddingHorizontal: 10, paddingVertical: 4 },
  deleteBtnText: { color: Colors.danger, fontSize: 22, fontWeight: '700' },

  loadingBlock:  { flex: 1, alignItems: 'center', justifyContent: 'center' },

  // Stats card
  statsCard:     { backgroundColor: Colors.surface, borderRadius: 4, padding: 14, marginBottom: 12, borderLeftWidth: 2, borderLeftColor: Colors.accent },
  statsRow:      { flexDirection: 'row', marginBottom: 10 },
  stat:          {},
  statLabel:     { color: Colors.textSecondary, fontSize: 9, letterSpacing: 2, fontFamily: 'monospace', marginBottom: 2 },
  statValue:     { color: Colors.textPrimary, fontSize: 14, fontFamily: 'monospace', fontWeight: '800' },

  modeMixRow:    { flexDirection: 'row', alignItems: 'baseline', marginTop: 2 },
  modeMixLabel:  { color: Colors.textSecondary, fontSize: 9, letterSpacing: 2, fontFamily: 'monospace', marginRight: 8, width: 38 },
  modeMixValue:  { color: Colors.textPrimary, fontSize: 12, fontFamily: 'monospace', fontWeight: '700' },

  captureBadge:     { marginTop: 10, alignSelf: 'flex-start', backgroundColor: 'rgba(52,199,89,0.15)', borderColor: Colors.success, borderWidth: 1, borderRadius: 3, paddingVertical: 4, paddingHorizontal: 8 },
  captureBadgeText: { color: Colors.success, fontSize: 10, letterSpacing: 2, fontFamily: 'monospace', fontWeight: '800' },

  // Map
  mapBlock:           { flex: 1, borderRadius: 4, overflow: 'hidden', marginBottom: 12, backgroundColor: Colors.surface },
  map:                { flex: 1 },
  mapPlaceholder:     { flex: 1, alignItems: 'center', justifyContent: 'center', padding: 24 },
  mapPlaceholderTitle:{ color: Colors.textSecondary, fontSize: 12, letterSpacing: 4, fontFamily: 'monospace', fontWeight: '800', marginBottom: 8 },
  mapPlaceholderBody: { color: Colors.textSecondary, fontSize: 11, fontFamily: 'monospace', textAlign: 'center', lineHeight: 16 },

  // Share
  shareBtn:      { backgroundColor: Colors.accent, paddingVertical: 14, borderRadius: 4, alignItems: 'center' },
  shareBtnText:  { color: '#000', fontWeight: '800', fontSize: 13, letterSpacing: 4, fontFamily: 'monospace' },

  footnote:      { color: Colors.textSecondary, fontSize: 10, fontFamily: 'monospace', letterSpacing: 1, textAlign: 'center', marginTop: 8, marginBottom: 8 },
});
