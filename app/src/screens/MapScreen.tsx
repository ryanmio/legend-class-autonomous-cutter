import React, { useRef, useEffect, useState, useCallback, useMemo } from 'react';
import { View, TouchableOpacity, Text, StyleSheet, Alert } from 'react-native';
import { WebView } from 'react-native-webview';
import { useSafeAreaInsets } from 'react-native-safe-area-context';
import { NativeStackScreenProps } from '@react-navigation/native-stack';
import * as Haptics from 'expo-haptics';
import { RootStackParamList } from '../../App';
import { Colors } from '../constants';
import { useTelemetry } from '../hooks/useTelemetry';
import {
  setWaypoint as sendWaypoint,
  setCruise as sendCruise,
  setMission as sendMission,
  getMission as fetchMission,
  clearMission as sendClearMission,
  LatLon,
} from '../services/esp32Service';
import { CruiseModal } from '../components/CruiseModal';

type Props = NativeStackScreenProps<RootStackParamList, 'Map'>;

const MAX_WAYPOINTS = 32;   // mirrors firmware config.h

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

function fmtDist(m: number): string {
  return m >= 1000 ? (m / 1000).toFixed(2) + ' km' : Math.round(m) + ' m';
}

// ── Leaflet map HTML ──────────────────────────────────────────────────────────
// Map-side rendering: boat marker + trail, the active MISSION (numbered markers
// + route polyline + bright dashed active-leg line), and the PLAN-mode DRAFT
// (amber numbered markers you tap to select/move/delete). A single waypoint is
// just a 1-point mission. HUD states: hidden / minimal (no fix) / card
// (tracking a leg or complete).
//
// Colors: live mission = bright cyan (active target); done legs = green/dim;
// draft = amber (clearly "not sent yet"); boat trail = muted steel blue.
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
      position:absolute; bottom:80px; left:50%; transform:translateX(-50%);
      font-family:monospace; text-align:center; z-index:1000; pointer-events:none;
    }
    #hud.hud-hidden  { display:none; }
    #hud.hud-minimal { padding:5px 12px; }
    #hud.hud-minimal #hud-primary{ font-size:11px; font-weight:700; letter-spacing:1.5px; }
    #hud.hud-minimal #hud-secondary{ display:none; }
    #hud.hud-card{
      background:rgba(10,15,26,0.92); padding:14px 22px; border-radius:6px;
      border:1px solid rgba(0,191,255,0.25); min-width:220px; max-width:78%;
    }
    #hud.hud-card #hud-primary{
      font-size:22px; font-weight:800; letter-spacing:1px; line-height:1.15; white-space:nowrap;
    }
    #hud.hud-card #hud-secondary{
      font-size:10px; color:#6b8fa8; letter-spacing:1.5px; margin-top:8px; font-weight:600;
    }

    .boat{font-size:22px;line-height:1}

    #layer-toggle{
      position:absolute; top:90px; left:12px;
      background:rgba(10,15,26,0.92); border:1.5px solid #00bfff; border-radius:4px;
      color:#00bfff; font-family:monospace; font-weight:800; font-size:11px;
      letter-spacing:2px; padding:7px 11px; z-index:1000; cursor:pointer; user-select:none;
    }

    /* Numbered waypoint markers, styled by state. */
    .wp-icon{background:transparent !important;border:none !important}
    .wpnum{
      width:24px;height:24px;border-radius:50%;
      display:flex;align-items:center;justify-content:center;
      font-family:monospace;font-weight:800;font-size:12px;
      border:2px solid #00bfff;color:#00bfff;background:rgba(10,15,26,0.85);
    }
    .wpnum-active{background:#00bfff;color:#08131f;width:28px;height:28px;font-size:13px;
      box-shadow:0 0 10px rgba(0,191,255,0.9);}
    .wpnum-done{border-color:#34c759;color:#34c759;opacity:0.55;}
    .wpnum-upcoming{}
    .wpnum-draft{border-color:#ffb020;color:#ffb020;}
    .wpnum-draft-sel{border-color:#ffd27f;color:#08131f;background:#ffb020;
      width:30px;height:30px;box-shadow:0 0 12px rgba(255,176,32,0.95);}
  </style>
</head>
<body>
  <div id="map"></div>
  <div id="hud" class="hud-hidden">
    <div id="hud-primary"></div>
    <div id="hud-secondary"></div>
  </div>
  <div id="layer-toggle">MAP</div>
  <script>
    var map = L.map('map', { zoomControl:false }).setView([37.8,-122.4], 13);

    var satLayer = L.tileLayer(
      'https://server.arcgisonline.com/ArcGIS/rest/services/World_Imagery/MapServer/tile/{z}/{y}/{x}',
      { attribution:'© Esri', maxZoom:19 }
    );
    var osmLayer = L.tileLayer('https://{s}.tile.openstreetmap.org/{z}/{x}/{y}.png', {
      attribution:'© OpenStreetMap contributors', maxZoom:19
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

    var boatIcon = L.divIcon({ html:'<div class="boat">⛵</div>', iconSize:[28,28], iconAnchor:[14,14], className:'' });

    function numIcon(num, state) {
      return L.divIcon({
        html:'<div class="wpnum wpnum-'+state+'">'+num+'</div>',
        iconSize:[24,24], iconAnchor:[12,12], className:'wp-icon'
      });
    }

    var marker   = null;
    var pathLine = null;              // bright dashed boat → active waypoint
    var trail    = L.polyline([], { color:'#3a6db8', weight:2.5, opacity:0.75 }).addTo(map);
    var pts      = [];
    var locked   = false;
    var tapMode  = 'live';

    var missionMarkers = [], missionLine = null, activeWp = null;
    var draftMarkers   = [], draftLine   = null;

    function post(o){ window.ReactNativeWebView.postMessage(JSON.stringify(o)); }

    function setHud(mode, primary, secondary, color) {
      var card = document.getElementById('hud');
      card.className = 'hud-' + mode;
      if (mode === 'hidden') return;
      var p = document.getElementById('hud-primary');
      p.textContent = primary || '';
      p.style.color = color || '#e8f4fd';
      document.getElementById('hud-secondary').textContent = secondary || '';
    }

    function updatePathLine() {
      if (marker && activeWp) {
        var coords = [marker.getLatLng(), activeWp];
        if (pathLine) pathLine.setLatLngs(coords);
        else pathLine = L.polyline(coords, { color:'#00bfff', weight:2.5, dashArray:'8, 6', opacity:0.9 }).addTo(map);
      } else if (pathLine) {
        map.removeLayer(pathLine); pathLine = null;
      }
    }

    window.setTapMode = function(mode){ tapMode = mode; };

    // Live mission: numbered markers + route polyline + active-leg dashed line.
    window.setMission = function(list, activeIdx){
      missionMarkers.forEach(function(m){ map.removeLayer(m); }); missionMarkers = [];
      if (missionLine){ map.removeLayer(missionLine); missionLine = null; }
      activeWp = null;
      if (!list || !list.length){ updatePathLine(); return; }
      var latlngs = [];
      for (var i=0;i<list.length;i++){
        var ll = [list[i].lat, list[i].lon]; latlngs.push(ll);
        var state = i < activeIdx ? 'done' : (i === activeIdx ? 'active' : 'upcoming');
        var m = L.marker(ll, { icon:numIcon(i+1, state), interactive:false }).addTo(map);
        missionMarkers.push(m);
        if (i === activeIdx) activeWp = ll;
      }
      if (list.length > 1) missionLine = L.polyline(latlngs, { color:'#00bfff', weight:2, opacity:0.5 }).addTo(map);
      updatePathLine();
    };
    window.clearMission = function(){
      missionMarkers.forEach(function(m){ map.removeLayer(m); }); missionMarkers = [];
      if (missionLine){ map.removeLayer(missionLine); missionLine = null; }
      activeWp = null; updatePathLine();
    };

    // Draft (PLAN mode): amber numbered markers, tap to select. selIdx<0 = none.
    window.setDraft = function(list, selIdx){
      draftMarkers.forEach(function(m){ map.removeLayer(m); }); draftMarkers = [];
      if (draftLine){ map.removeLayer(draftLine); draftLine = null; }
      if (!list || !list.length) return;
      var latlngs = [];
      for (var i=0;i<list.length;i++){
        var ll = [list[i].lat, list[i].lon]; latlngs.push(ll);
        var state = (i === selIdx) ? 'draft-sel' : 'draft';
        var m = L.marker(ll, { icon:numIcon(i+1, state) }).addTo(map);
        (function(idx){
          m.on('click', function(ev){ L.DomEvent.stopPropagation(ev); post({ type:'draftmarker', index:idx }); });
        })(i);
        draftMarkers.push(m);
      }
      draftLine = L.polyline(latlngs, { color:'#ffb020', weight:2.5, dashArray:'6, 6', opacity:0.85 }).addTo(map);
    };
    window.clearDraft = function(){
      draftMarkers.forEach(function(m){ map.removeLayer(m); }); draftMarkers = [];
      if (draftLine){ map.removeLayer(draftLine); draftLine = null; }
    };

    window.updateBoat = function(lat, lon, hasFix, bearing, distM, complete, legText, completeLabel) {
      if (!hasFix) { setHud('minimal', '⚠ NO GPS FIX', '', '#ffcc00'); return; }
      var ll = [lat, lon];
      if (!marker) { marker = L.marker(ll, { icon:boatIcon }).addTo(map); map.setView(ll, 18); }
      else { marker.setLatLng(ll); if (locked) map.panTo(ll, { animate:true, duration:0.5 }); }
      pts.push(ll); if (pts.length > 800) pts.shift();
      trail.setLatLngs(pts);
      updatePathLine();

      var llStr = lat.toFixed(6) + ', ' + lon.toFixed(6);
      if (complete) {
        setHud('card', '✓ ' + (completeLabel || 'CAPTURED'), llStr, '#34c759');
      } else if (bearing != null && distM != null && !isNaN(bearing)) {
        var d = distM >= 1000 ? (distM / 1000).toFixed(2) + ' km' : Math.round(distM) + ' m';
        setHud('card', (legText || '') + '→ ' + bearing.toFixed(0) + '°  ·  ' + d, llStr, '#34c759');
      } else {
        setHud('hidden');
      }
    };

    window.centerOnBoat = function() {
      if (marker) { map.setView(marker.getLatLng(), map.getZoom()); locked = true; }
    };

    map.on('dragstart', function() { locked = false; });

    map.on('click', function(e) {
      if (tapMode === 'plan') post({ type:'plantap', lat:e.latlng.lat, lon:e.latlng.lng });
      else                    post({ type:'waypoint', lat:e.latlng.lat, lon:e.latlng.lng });
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
  const webViewRef = useRef<WebView>(null);
  const insets     = useSafeAreaInsets();

  // The mission the app believes is on the boat (authoritative after any local
  // action; rehydrated from GET /mission once per mount).
  const [missionRoute, setMissionRoute] = useState<LatLon[]>([]);
  const [screenMode, setScreenMode]     = useState<'live' | 'plan'>('live');
  const [draft, setDraft]               = useState<LatLon[]>([]);
  const [selectedIdx, setSelectedIdx]   = useState<number | null>(null);
  const [cruiseModalOpen, setCruiseModalOpen] = useState(false);
  const [webViewReady, setWebViewReady] = useState(false);
  // After a local mission change the boat resets to leg 0; until telemetry shows
  // that, don't trust its wp_idx/captured (count-equality can't catch a same-count
  // re-plan). State (not a ref) so GET resolution re-runs the fallback-hydrate.
  const [expectReset, setExpectReset]   = useState(false);
  const [missionFetch, setMissionFetch] = useState<'pending' | 'done' | 'fail'>('pending');

  const inPlan = screenMode === 'plan';

  // Active leg index from telemetry, clamped to the local route length (guards
  // the window right after CONFIRM where telemetry still reflects the old route).
  const activeIdx = Math.min(
    Math.max(0, data?.wp_idx ?? 0),
    Math.max(0, missionRoute.length - 1),
  );

  // Telemetry lags the local route by ~1 poll after a local mission change (it
  // still reports the previous mission's wp_count/wp_idx/captured). Count-equality
  // alone can't detect a same-count re-plan, so ALSO require that the boat has
  // reset to leg 0 since our last change (expectReset). Until synced, treat the
  // active leg as 0 and the mission as not-done — otherwise a fresh mission
  // briefly shows "MISSION COMPLETE"/wrong LEG, and (worse) re-entering PLAN in
  // that window would slice the route at a stale index and ship a truncated one.
  // hasMissionTelemetry keeps this graceful against pre-0.7.0 firmware (no
  // wp_count/wp_idx) — there we simply trust telemetry as the old single-WP app did.
  const hasMissionTelemetry = data?.wp_count != null;
  const telemInSync   = !hasMissionTelemetry || (data!.wp_count === missionRoute.length && !expectReset);
  const activeIdxSafe = telemInSync ? activeIdx : 0;
  const missionDone   = telemInSync && data?.captured === true && missionRoute.length > 0;

  // Rehydrate the route ONCE per mount. GET /mission is authoritative; if it
  // fails (older firmware / offline) fall back to the single active waypoint
  // from telemetry. Any local action also flips the ref so a hydrate can't
  // stomp a fresh user edit.
  const hydratedRef = useRef(false);
  const markHydrated = () => { hydratedRef.current = true; };

  // GET /mission is authoritative for the full route. The telemetry fallback
  // (single active waypoint) runs ONLY if GET failed — otherwise the cached last
  // frame, delivered synchronously on mount, wins the race and collapses a
  // multi-waypoint route to one point before GET resolves. missionFetch is state
  // so its 'fail' transition re-renders and re-runs the fallback effect (a ref
  // wouldn't, and a stationary boat's wp_lat/wp_lon never change to retrigger it).
  useEffect(() => {
    if (hydratedRef.current) return;
    fetchMission(ip)
      .then((m) => {
        if (!hydratedRef.current) {
          const pts = (m.waypoints ?? [])
            .map((w) => ({ lat: parseFloat(w.lat), lon: parseFloat(w.lon) }))
            .filter((p) => !isNaN(p.lat) && !isNaN(p.lon));
          if (pts.length) {
            markHydrated();
            setMissionRoute(pts);
          }
        }
        setMissionFetch('done');
      })
      .catch(() => setMissionFetch('fail'));
  }, [ip]);

  useEffect(() => {
    if (hydratedRef.current) return;
    if (missionFetch !== 'fail') return;   // wait for GET; only fall back if it failed
    if (!data?.wp_set || data.wp_lat == null || data.wp_lon == null) return;
    const lat = parseFloat(data.wp_lat);
    const lon = parseFloat(data.wp_lon);
    if (isNaN(lat) || isNaN(lon)) return;
    markHydrated();
    setMissionRoute([{ lat, lon }]);
  }, [missionFetch, data?.wp_set, data?.wp_lat, data?.wp_lon]);

  // A fresh mission commits at leg 0 on the boat; once telemetry shows that, our
  // local route and the boat's are back in sync. (On pre-0.7.0 firmware wp_idx is
  // absent and expectReset is ignored by telemInSync, so it never blocks.)
  useEffect(() => {
    if (expectReset && data?.wp_idx === 0) setExpectReset(false);
  }, [expectReset, data?.wp_idx]);

  // Reflect the live mission into the WebView (hidden while planning).
  useEffect(() => {
    if (!webViewReady || !webViewRef.current) return;
    if (inPlan) {
      webViewRef.current.injectJavaScript('window.clearMission();true;');
    } else if (missionRoute.length) {
      // On completion, pass an index past the end so every marker renders "done"
      // (green ✓) rather than leaving the last one bright-cyan "active".
      const renderIdx = missionDone ? missionRoute.length : activeIdxSafe;
      webViewRef.current.injectJavaScript(
        `window.setMission(${JSON.stringify(missionRoute)},${renderIdx});true;`
      );
    } else {
      webViewRef.current.injectJavaScript('window.clearMission();true;');
    }
  }, [missionRoute, activeIdxSafe, missionDone, inPlan, webViewReady]);

  // Reflect the draft into the WebView (PLAN mode only).
  useEffect(() => {
    if (!webViewReady || !webViewRef.current) return;
    if (inPlan) {
      webViewRef.current.injectJavaScript(
        `window.setDraft(${JSON.stringify(draft)},${selectedIdx ?? -1});true;`
      );
    } else {
      webViewRef.current.injectJavaScript('window.clearDraft();true;');
    }
  }, [draft, selectedIdx, inPlan, webViewReady]);

  // Tell the map how a tap should be interpreted.
  useEffect(() => {
    if (!webViewReady || !webViewRef.current) return;
    webViewRef.current.injectJavaScript(`window.setTapMode('${screenMode}');true;`);
  }, [screenMode, webViewReady]);

  // Boat position + HUD (active leg / mission complete).
  useEffect(() => {
    if (!webViewReady || !webViewRef.current || !data) return;
    const lat    = parseFloat(data.lat ?? '');
    const lon    = parseFloat(data.lon ?? '');
    const hasFix = data.gps_fix === true && !isNaN(lat) && !isNaN(lon);
    // n from the LOCAL route (authoritative); complete/index guarded by telemInSync.
    const n = missionRoute.length;

    let bearing: number | null = null;
    let dist:    number | null = null;
    const active = missionRoute[activeIdxSafe];
    if (hasFix && active) {
      bearing = bearingTo(lat, lon, active.lat, active.lon);
      dist    = distanceTo(lat, lon, active.lat, active.lon);
    }
    const legText       = n > 1 ? `LEG ${activeIdxSafe + 1}/${n}  ` : '';
    const completeLabel = n > 1 ? 'MISSION COMPLETE' : 'CAPTURED';

    webViewRef.current.injectJavaScript(
      `window.updateBoat(${hasFix ? lat : 0},${hasFix ? lon : 0},${hasFix},` +
      `${bearing ?? 'null'},${dist ?? 'null'},${missionDone},` +
      `${JSON.stringify(legText)},${JSON.stringify(completeLabel)});true;`
    );
  }, [data?.gps_fix, data?.lat, data?.lon, missionDone, activeIdxSafe, missionRoute, webViewReady]);

  // ── Map tap messages ─────────────────────────────────────────────
  const applySingleWaypoint = useCallback((pt: LatLon) => {
    Haptics.impactAsync(Haptics.ImpactFeedbackStyle.Medium);
    markHydrated();
    setMissionRoute([pt]);
    setExpectReset(true);
    sendWaypoint(ip, pt.lat, pt.lon).catch(() => {});
  }, [ip]);

  const handleMapMessage = useCallback((raw: string) => {
    let msg: any;
    try { msg = JSON.parse(raw); } catch { return; }

    if (msg.type === 'waypoint') {
      // LIVE tap → single waypoint. Ignore if we've switched to PLAN but the
      // setTapMode injection hasn't landed yet (stale tap during the switch).
      if (inPlan) return;
      // Gate the override if a multi-leg mission is running, so a stray tap
      // can't wipe a planned route.
      const pt: LatLon = { lat: msg.lat, lon: msg.lon };
      if (missionRoute.length > 1) {
        Alert.alert(
          'Replace mission?',
          `Drop the ${missionRoute.length}-waypoint mission and steer to a single point here?`,
          [
            { text: 'Cancel', style: 'cancel' },
            { text: 'Replace', style: 'destructive', onPress: () => applySingleWaypoint(pt) },
          ],
        );
      } else {
        applySingleWaypoint(pt);
      }
      return;
    }

    if (msg.type === 'plantap') {
      if (!inPlan) return;   // stale tap after leaving PLAN before setTapMode('live') landed
      Haptics.impactAsync(Haptics.ImpactFeedbackStyle.Light);
      if (selectedIdx != null) {
        // Move the selected point here (stays selected for further nudging).
        setDraft((d) => d.map((p, i) => (i === selectedIdx ? { lat: msg.lat, lon: msg.lon } : p)));
      } else {
        setDraft((d) => {
          if (d.length >= MAX_WAYPOINTS) {
            Alert.alert('Mission full', `Maximum ${MAX_WAYPOINTS} waypoints.`);
            return d;
          }
          return [...d, { lat: msg.lat, lon: msg.lon }];
        });
      }
      return;
    }

    if (msg.type === 'draftmarker') {
      Haptics.impactAsync(Haptics.ImpactFeedbackStyle.Medium);
      setSelectedIdx((cur) => (cur === msg.index ? null : msg.index));
    }
  }, [ip, inPlan, missionRoute, selectedIdx, applySingleWaypoint]);

  // ── PLAN mode actions ────────────────────────────────────────────
  const enterPlan = useCallback(() => {
    Haptics.impactAsync(Haptics.ImpactFeedbackStyle.Medium);
    // Pre-load for editing: the remaining (uncaptured) legs mid-mission, or the
    // WHOLE route once complete (so "re-run" resends everything, not just the
    // final point the active index is pinned to).
    setDraft(missionRoute.slice(missionDone ? 0 : activeIdxSafe));
    setSelectedIdx(null);
    setScreenMode('plan');
  }, [missionRoute, activeIdxSafe, missionDone]);

  const cancelPlan = useCallback(() => {
    setScreenMode('live');
    setDraft([]);
    setSelectedIdx(null);
  }, []);

  const confirmPlan = useCallback(() => {
    if (draft.length === 0) { Alert.alert('Empty route', 'Add at least one waypoint.'); return; }
    Haptics.impactAsync(Haptics.ImpactFeedbackStyle.Heavy);
    const route = draft;
    sendMission(ip, route)
      .then(() => {
        markHydrated();
        setMissionRoute(route);
        setExpectReset(true);   // boat restarts at leg 0; don't trust stale telemetry until it shows that
        setScreenMode('live');
        setDraft([]);
        setSelectedIdx(null);
      })
      .catch((e: any) => {
        Alert.alert('Route rejected', e?.message ?? 'The boat rejected the mission.');
        if (typeof e?.bad_leg === 'number') setSelectedIdx(Math.min(e.bad_leg, route.length - 1));
      });
  }, [ip, draft]);

  const deleteSelected = useCallback(() => {
    if (selectedIdx == null) return;
    Haptics.impactAsync(Haptics.ImpactFeedbackStyle.Medium);
    setDraft((d) => d.filter((_, i) => i !== selectedIdx));
    setSelectedIdx(null);
  }, [selectedIdx]);

  const undoLast   = useCallback(() => { setDraft((d) => d.slice(0, -1)); setSelectedIdx(null); }, []);
  const clearDraft = useCallback(() => { setDraft([]); setSelectedIdx(null); }, []);

  const handleClearMission = useCallback(() => {
    Haptics.impactAsync(Haptics.ImpactFeedbackStyle.Medium);
    markHydrated();
    setMissionRoute([]);
    sendClearMission(ip).catch(() => {});
  }, [ip]);

  const handlePickCruise = useCallback((us: number) => {
    sendCruise(ip, { us }).catch(() => {});
    setCruiseModalOpen(false);
  }, [ip]);

  const draftDist = useMemo(() => {
    let s = 0;
    for (let i = 1; i < draft.length; i++) s += distanceTo(draft[i - 1].lat, draft[i - 1].lon, draft[i].lat, draft[i].lon);
    return s;
  }, [draft]);

  const mode      = data?.mode;
  const failsafe  = mode === 'FAILSAFE';
  const ackNeeded = data?.failsafe_ack === true;
  const cruiseUs  = data?.cruise_us;
  const topOffset = (failsafe ? insets.top + 40 : insets.top);

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

      {failsafe && (
        <View style={[styles.failsafeBanner, { paddingTop: insets.top + 8 }]}>
          <Text style={styles.failsafeText}>
            {ackNeeded ? 'FAILSAFE · ACK_REQUIRED — flip SwA UP' : 'FAILSAFE'}
          </Text>
        </View>
      )}

      {/* ── Top bar: MODE + PLAN badge ─────────────────────────────── */}
      <View style={[styles.topBarRow, { top: topOffset + 8 }]} pointerEvents="box-none">
        <View style={styles.topBarSpacer} />
        {inPlan && (
          <View style={[styles.chip, styles.modeChip, { borderColor: AMBER }]}>
            <Text style={[styles.modeChipText, { color: AMBER }]}>PLAN</Text>
          </View>
        )}
        {mode && (
          <View style={[styles.chip, styles.modeChip, { borderColor: modeColor(mode) }]}>
            <Text style={[styles.chipDot, { color: modeColor(mode) }]}>●</Text>
            <Text style={[styles.modeChipText, { color: modeColor(mode) }]}>{mode}</Text>
          </View>
        )}
      </View>

      {/* ── Action bar (LIVE): cruise · plan · clear ───────────────── */}
      {!inPlan && (
        <View style={[styles.actionBarRow, { top: topOffset + 50 }]} pointerEvents="box-none">
          <TouchableOpacity style={styles.cruisePill} onPress={() => setCruiseModalOpen(true)} activeOpacity={0.7}>
            <Text style={styles.cruisePillLabel}>CRUISE</Text>
            <Text style={styles.cruisePillValue}>{cruiseUs ?? '—'}</Text>
            <Text style={styles.cruisePillChevron}>▾</Text>
          </TouchableOpacity>

          <TouchableOpacity style={styles.planPill} onPress={enterPlan} activeOpacity={0.7}>
            <Text style={styles.planPillText}>⊹ PLAN</Text>
          </TouchableOpacity>

          {missionRoute.length > 0 && (
            <TouchableOpacity style={styles.wpClearBtn} onPress={handleClearMission} activeOpacity={0.7}>
              <Text style={styles.wpClearBtnText}>✕ CLEAR</Text>
            </TouchableOpacity>
          )}
        </View>
      )}

      {/* ── PLAN selection banner ──────────────────────────────────── */}
      {inPlan && selectedIdx != null && (
        <View style={[styles.selBanner, { top: topOffset + 50 }]} pointerEvents="box-none">
          <Text style={styles.selBannerText}>MOVING #{selectedIdx + 1} — tap map to reposition</Text>
        </View>
      )}

      {/* ── PLAN toolbar (bottom) ──────────────────────────────────── */}
      {inPlan && (
        <View style={[styles.planBar, { bottom: insets.bottom + 88 }]}>
          <Text style={styles.planBarStat}>
            {draft.length} pt{draft.length === 1 ? '' : 's'} · ~{fmtDist(draftDist)}
          </Text>
          <View style={styles.planBtnRow}>
            {selectedIdx != null ? (
              <>
                <TouchableOpacity style={[styles.planBtn, styles.planBtnDanger]} onPress={deleteSelected} activeOpacity={0.7}>
                  <Text style={styles.planBtnDangerText}>DELETE #{selectedIdx + 1}</Text>
                </TouchableOpacity>
                <TouchableOpacity style={styles.planBtn} onPress={() => setSelectedIdx(null)} activeOpacity={0.7}>
                  <Text style={styles.planBtnText}>DONE</Text>
                </TouchableOpacity>
              </>
            ) : (
              <>
                <TouchableOpacity
                  style={[styles.planBtn, draft.length === 0 && styles.planBtnDisabled]}
                  onPress={undoLast}
                  disabled={draft.length === 0}
                  activeOpacity={0.7}
                >
                  <Text style={styles.planBtnText}>UNDO</Text>
                </TouchableOpacity>
                <TouchableOpacity
                  style={[styles.planBtn, draft.length === 0 && styles.planBtnDisabled]}
                  onPress={clearDraft}
                  disabled={draft.length === 0}
                  activeOpacity={0.7}
                >
                  <Text style={styles.planBtnText}>CLEAR</Text>
                </TouchableOpacity>
                <TouchableOpacity style={styles.planBtn} onPress={cancelPlan} activeOpacity={0.7}>
                  <Text style={styles.planBtnText}>CANCEL</Text>
                </TouchableOpacity>
                <TouchableOpacity
                  style={[styles.planBtn, styles.planBtnGo, draft.length === 0 && styles.planBtnDisabled]}
                  onPress={confirmPlan}
                  disabled={draft.length === 0}
                  activeOpacity={0.7}
                >
                  <Text style={styles.planBtnGoText}>CONFIRM ▸</Text>
                </TouchableOpacity>
              </>
            )}
          </View>
        </View>
      )}

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
const AMBER   = '#ffb020';

const styles = StyleSheet.create({
  screen: { flex: 1, backgroundColor: Colors.background },
  map:    { flex: 1 },

  failsafeBanner: { position: 'absolute', top: 0, left: 0, right: 0, backgroundColor: Colors.danger, paddingBottom: 8, alignItems: 'center', zIndex: 100 },
  failsafeText:   { color: '#fff', fontSize: 12, fontWeight: '800', letterSpacing: 2, fontFamily: 'monospace' },

  topBarRow:      { position: 'absolute', left: 12, right: 12, flexDirection: 'row', alignItems: 'center', gap: 6 },
  topBarSpacer:   { flex: 1 },
  chip:           { flexDirection: 'row', alignItems: 'center', backgroundColor: CHIP_BG, paddingHorizontal: 10, paddingVertical: 6, borderRadius: 4 },
  chipDot:        { fontSize: 9, marginRight: 5 },
  modeChip:       { borderWidth: 1.5, paddingVertical: 5 },
  modeChipText:   { fontSize: 11, fontFamily: 'monospace', fontWeight: '800', letterSpacing: 2 },

  actionBarRow:      { position: 'absolute', left: 12, right: 12, flexDirection: 'row', alignItems: 'center', gap: 8 },
  cruisePill:        { flexDirection: 'row', alignItems: 'center', backgroundColor: CHIP_BG, paddingHorizontal: 12, paddingVertical: 8, borderRadius: 4, borderLeftWidth: 2, borderLeftColor: Colors.accent },
  cruisePillLabel:   { color: Colors.textSecondary, fontSize: 10, fontFamily: 'monospace', letterSpacing: 2, fontWeight: '700', marginRight: 8 },
  cruisePillValue:   { color: Colors.accent, fontSize: 13, fontFamily: 'monospace', fontWeight: '800', letterSpacing: 1 },
  cruisePillChevron: { color: Colors.accent, fontSize: 11, marginLeft: 6 },

  planPill:     { backgroundColor: CHIP_BG, paddingHorizontal: 12, paddingVertical: 8, borderRadius: 4, borderLeftWidth: 2, borderLeftColor: AMBER },
  planPillText: { color: AMBER, fontSize: 12, fontFamily: 'monospace', fontWeight: '800', letterSpacing: 1 },

  wpClearBtn:     { backgroundColor: 'rgba(255,59,48,0.92)', paddingHorizontal: 12, paddingVertical: 8, borderRadius: 4 },
  wpClearBtnText: { color: '#fff', fontSize: 11, fontFamily: 'monospace', fontWeight: '800', letterSpacing: 1 },

  // PLAN selection banner
  selBanner:     { position: 'absolute', left: 12, right: 12, alignItems: 'center' },
  selBannerText: { backgroundColor: 'rgba(255,176,32,0.95)', color: '#08131f', fontSize: 12, fontFamily: 'monospace', fontWeight: '800', letterSpacing: 1, paddingHorizontal: 14, paddingVertical: 7, borderRadius: 4, overflow: 'hidden' },

  // PLAN bottom toolbar
  planBar:     { position: 'absolute', left: 12, right: 12, backgroundColor: 'rgba(10,15,26,0.94)', borderRadius: 8, borderWidth: 1, borderColor: 'rgba(255,176,32,0.4)', paddingHorizontal: 12, paddingVertical: 10, gap: 8 },
  planBarStat: { color: AMBER, fontSize: 12, fontFamily: 'monospace', fontWeight: '800', letterSpacing: 1, textAlign: 'center' },
  planBtnRow:  { flexDirection: 'row', flexWrap: 'wrap', justifyContent: 'center', gap: 8 },
  planBtn:         { paddingHorizontal: 14, paddingVertical: 9, borderRadius: 4, borderWidth: 1, borderColor: Colors.accent, backgroundColor: 'rgba(0,191,255,0.08)' },
  planBtnText:     { color: Colors.accent, fontSize: 12, fontFamily: 'monospace', fontWeight: '800', letterSpacing: 1 },
  planBtnDisabled: { opacity: 0.35 },
  planBtnGo:       { borderColor: Colors.success, backgroundColor: 'rgba(52,199,89,0.15)' },
  planBtnGoText:   { color: Colors.success, fontSize: 12, fontFamily: 'monospace', fontWeight: '800', letterSpacing: 1 },
  planBtnDanger:     { borderColor: Colors.danger, backgroundColor: 'rgba(255,59,48,0.15)' },
  planBtnDangerText: { color: Colors.danger, fontSize: 12, fontFamily: 'monospace', fontWeight: '800', letterSpacing: 1 },

  centerFab: {
    position: 'absolute', right: 16, width: 52, height: 52, borderRadius: 26,
    backgroundColor: 'rgba(10,15,26,0.92)', borderWidth: 1.5, borderColor: Colors.accent,
    alignItems: 'center', justifyContent: 'center',
    shadowColor: '#000', shadowOpacity: 0.4, shadowOffset: { width: 0, height: 2 }, shadowRadius: 4, elevation: 4,
  },
  centerFabIcon: { color: Colors.accent, fontSize: 24, fontWeight: '800', lineHeight: 24 },

  backFab: {
    position: 'absolute', left: 16, height: 52, paddingHorizontal: 18, borderRadius: 26,
    backgroundColor: 'rgba(10,15,26,0.92)', borderWidth: 1.5, borderColor: Colors.accent,
    alignItems: 'center', justifyContent: 'center',
    shadowColor: '#000', shadowOpacity: 0.4, shadowOffset: { width: 0, height: 2 }, shadowRadius: 4, elevation: 4,
  },
  backFabText: { color: Colors.accent, fontSize: 13, fontFamily: 'monospace', fontWeight: '800', letterSpacing: 2 },
});
