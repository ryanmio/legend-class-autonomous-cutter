import React, { useCallback, useEffect, useState } from 'react';
import { View, Text, StyleSheet, TouchableOpacity } from 'react-native';
import { NativeStackScreenProps } from '@react-navigation/native-stack';
import { useKeepAwake } from 'expo-keep-awake';
import * as Haptics from 'expo-haptics';
import { RootStackParamList } from '../../App';
import { Colors } from '../constants';
import { BATT_LOW_V, BATT_CRIT_V, TelemetryData } from '../types';
import { useTelemetry } from '../hooks/useTelemetry';
import { setLed, setCruise as sendCruise, playAudio } from '../services/esp32Service';
import { subscribeRunning } from '../services/telemetryLogger';
import { CruiseModal } from '../components/CruiseModal';
import Screen from '../components/Screen';

type Props = NativeStackScreenProps<RootStackParamList, 'Helm'>;
type LightKey = 'nav' | 'bridge' | 'deck';
type SoundKey = 'horn' | 'board' | 'gun';

const LIGHTS: { key: LightKey; label: string }[] = [
  { key: 'nav',    label: 'NAV'    },
  { key: 'bridge', label: 'BRIDGE' },
  { key: 'deck',   label: 'DECK'   },
];

// test_29 plays track 1 for any sound key. Different tracks per key will
// land in firmware once we decide a mapping.
const SOUNDS: { key: SoundKey; label: string }[] = [
  { key: 'horn',  label: 'HORN'  },
  { key: 'board', label: 'BOARD' },
  { key: 'gun',   label: 'GUN'   },
];

const NAV: { screen: 'Map' | 'Telemetry' | 'Systems'; label: string }[] = [
  { screen: 'Map',       label: 'MAP'       },
  { screen: 'Telemetry', label: 'TELEMETRY' },
  { screen: 'Systems',   label: 'SYSTEMS'   },
];

// ── Helpers ────────────────────────────────────────────────────────────────────
function voltageColor(v: number | undefined): string | undefined {
  if (v == null || isNaN(v)) return undefined;
  if (v < BATT_CRIT_V) return Colors.danger;
  if (v < BATT_LOW_V)  return Colors.warning;
  return Colors.success;
}

function modeColor(mode: string | undefined): string {
  switch (mode) {
    case 'AUTO':     return Colors.success;
    case 'MANUAL':   return Colors.accent;
    case 'FAILSAFE': return Colors.danger;
    default:         return Colors.textSecondary;
  }
}

// One-line English summary of what the boat is doing right now. Mode
// (MANUAL/AUTO/FAILSAFE) is conveyed by the colored chip in the header,
// so this sentence describes the *condition* — never repeats the mode.
function stateSentence(
  data: TelemetryData | null,
  connected: boolean
): { text: string; color: string } {
  if (!connected) return { text: 'OFFLINE',     color: Colors.danger        };
  if (!data)      return { text: 'CONNECTING…', color: Colors.textSecondary };

  if (data.mode === 'FAILSAFE') {
    return {
      text:  data.failsafe_ack ? 'ACK_REQUIRED · FLIP SwA UP' : 'OUTPUTS NEUTRAL',
      color: Colors.danger,
    };
  }
  if (data.mode === 'AUTO') {
    if (!data.wp_set)  return { text: 'NO WAYPOINT', color: Colors.warning };
    if (!data.gps_fix) return { text: 'NO GPS FIX',  color: Colors.warning };
    if (data.captured) return { text: 'CAPTURED',    color: Colors.success };
    const dist = data.wp_dist_m != null ? ` · ${data.wp_dist_m} M` : '';
    return { text: `TRACKING WP${dist}`, color: Colors.success };
  }

  // MANUAL — speed-aware
  const sp = data.speed_kts != null ? parseFloat(data.speed_kts) : 0;
  if (!isNaN(sp) && sp > 0.2) {
    return { text: `UNDERWAY · ${sp.toFixed(1)} KTS`, color: Colors.accent };
  }
  return { text: 'AT REST', color: Colors.textSecondary };
}

// ── Screen ─────────────────────────────────────────────────────────────────────
export default function HelmScreen({ route, navigation }: Props) {
  const { ip } = route.params;
  const { data, connected } = useTelemetry();
  const [logging, setLogging]         = useState(false);
  const [cruiseModalOpen, setCruise]  = useState(false);
  // Local optimistic light state. test_29 doesn't echo nav_on/bridge_on/
  // deck_on; production firmware will. When it does, replace this with a
  // server-state-with-pending-override pattern.
  const [lights, setLights] = useState<Record<LightKey, boolean>>({
    nav: false, bridge: false, deck: false,
  });
  useKeepAwake();

  useEffect(() => subscribeRunning(setLogging), []);

  const battV    = data?.batt_v != null ? parseFloat(data.batt_v) : undefined;
  const cruiseUs = data?.cruise_us;
  const wpReady  = data?.wp_set && data?.wp_dist_m != null;
  const sentence = stateSentence(data, connected);

  const handlePickCruise = useCallback((us: number) => {
    sendCruise(ip, { us }).catch(() => {});
    setCruise(false);
  }, [ip]);

  const toggleLight = useCallback((key: LightKey) => {
    Haptics.impactAsync(Haptics.ImpactFeedbackStyle.Light);
    setLights((prev) => {
      const next = !prev[key];
      setLed(ip, key, next).catch(() => {});
      return { ...prev, [key]: next };
    });
  }, [ip]);

  const playSound = useCallback((key: SoundKey) => {
    Haptics.impactAsync(Haptics.ImpactFeedbackStyle.Light);
    playAudio(ip, key).catch(() => {});
  }, [ip]);

  // Bilge summary: count of wet zones (0..3). Drives the helm status
  // card; full controls live on SystemsScreen.
  const wetCount =
    (data?.bilge_fwd  ? 1 : 0) +
    (data?.bilge_mid  ? 1 : 0) +
    (data?.bilge_rear ? 1 : 0);

  return (
    <Screen>
      <View style={styles.inner}>

        {/* ── HEADER ────────────────────────────────────────────────── */}
        <View style={styles.header}>
          <View style={styles.brandCol}>
            <Text style={styles.brand}>LEGEND CUTTER</Text>
            {/* USCG stripe — single tasteful nod. */}
            <View style={styles.stripe}>
              <View style={[styles.stripeSeg, { backgroundColor: '#c8102e' }]} />
              <View style={[styles.stripeSeg, { backgroundColor: '#ffffff' }]} />
              <View style={[styles.stripeSeg, { backgroundColor: '#003a70' }]} />
            </View>
          </View>
          <View style={[styles.modeChip, { borderColor: modeColor(data?.mode) }]}>
            <Text style={[styles.modeDot, { color: modeColor(data?.mode) }]}>●</Text>
            <Text style={[styles.modeChipText, { color: modeColor(data?.mode) }]}>
              {data?.mode ?? 'IDLE'}
            </Text>
          </View>
        </View>

        <View style={styles.statusRow}>
          <Text style={[styles.statusDot, { color: connected ? Colors.success : Colors.danger }]}>●</Text>
          <Text style={styles.statusLabel}>{connected ? 'ONLINE' : 'OFFLINE'}</Text>
          {logging && (
            <>
              <Text style={styles.statusSep}>·</Text>
              <Text style={[styles.statusDot, { color: Colors.success }]}>●</Text>
              <Text style={styles.statusLabel}>LOG</Text>
            </>
          )}
          <Text style={styles.statusSep}>·</Text>
          <Text style={[styles.stateSentence, { color: sentence.color }]} numberOfLines={1}>
            {sentence.text}
          </Text>
        </View>

        {/* ── BILGE ALARM (conditional, top of screen) ───────────────── */}
        {(data?.bilge_fwd || data?.bilge_mid || data?.bilge_rear) && (
          <View style={styles.alarm}>
            <Text style={styles.alarmText}>
              ⚠ WATER DETECTED — PUMP {data?.pump ? 'ON' : 'OFF'}
            </Text>
          </View>
        )}

        {/* ── PRIMARY READOUTS ──────────────────────────────────────── */}
        <View style={styles.readoutRow}>
          <Readout
            label="HEADING"
            value={data?.heading != null ? `${Math.round(data.heading)}°` : '--'}
          />
          <Readout
            label="SPEED"
            value={data?.speed_kts != null ? `${data.speed_kts}` : '--'}
            unit={data?.speed_kts != null ? 'KTS' : undefined}
          />
          <Readout
            label="DEPTH"
            value={data?.depth_m != null ? `${data.depth_m}` : '--'}
            unit={data?.depth_m != null ? 'M' : undefined}
          />
          <Readout
            label="BATT"
            value={battV != null ? battV.toFixed(2) : '--'}
            unit={battV != null ? 'V' : undefined}
            color={voltageColor(battV)}
          />
        </View>

        {/* ── AUTOPILOT ─────────────────────────────────────────────── */}
        <Text style={styles.sectionLabel}>AUTOPILOT</Text>
        <View style={styles.autopilotRow}>
          <TouchableOpacity
            style={styles.apCard}
            onPress={() => setCruise(true)}
            activeOpacity={0.7}
          >
            <View style={styles.apCardHead}>
              <Text style={styles.apCardLabel}>CRUISE</Text>
              <Text style={styles.apCardChevron}>▾</Text>
            </View>
            <Text style={styles.apCardValue}>
              {cruiseUs != null ? `${cruiseUs}` : '--'}
              <Text style={styles.apCardUnit}>{cruiseUs != null ? ' µs' : ''}</Text>
            </Text>
            <Text style={styles.apCardSub}>tap to change</Text>
          </TouchableOpacity>

          <TouchableOpacity
            style={styles.apCard}
            onPress={() => navigation.navigate('Map', { ip })}
            activeOpacity={0.7}
          >
            <View style={styles.apCardHead}>
              <Text style={styles.apCardLabel}>WAYPOINT</Text>
              <Text style={styles.apCardChevron}>↗</Text>
            </View>
            <Text style={styles.apCardValue}>
              {wpReady ? `${data!.wp_dist_m}` : '--'}
              <Text style={styles.apCardUnit}>{wpReady ? ' M' : ''}</Text>
            </Text>
            <Text style={styles.apCardSub}>
              {wpReady && data!.wp_bearing != null
                ? `→ ${data!.wp_bearing}°`
                : 'tap to set on map'}
            </Text>
          </TouchableOpacity>
        </View>

        {/* ── BILGE STATUS (compact; full controls on SystemsScreen) ── */}
        <TouchableOpacity
          style={styles.bilgeCard}
          onPress={() => navigation.navigate('Systems', { ip })}
          activeOpacity={0.7}
        >
          <Text style={[styles.bilgeCardLabel, wetCount > 0 && styles.bilgeCardLabelWet]}>
            BILGE
          </Text>
          <Text style={[styles.bilgeCardValue, wetCount > 0 && styles.bilgeCardValueWet]}>
            {wetCount}/3 {wetCount > 0 ? 'FLOODED' : 'DRY'}
            {data?.pump ? ' · PUMP ON' : ''}
          </Text>
        </TouchableOpacity>

        {/* ── LIGHTS ────────────────────────────────────────────────── */}
        <Text style={styles.sectionLabel}>LIGHTS</Text>
        <View style={styles.lightsRow}>
          {LIGHTS.map(({ key, label }) => {
            const on = lights[key];
            return (
              <TouchableOpacity
                key={key}
                style={[styles.lightBtn, on && styles.lightBtnOn]}
                onPress={() => toggleLight(key)}
                activeOpacity={0.7}
              >
                <Text style={[styles.lightBtnText, on && styles.lightBtnTextOn]}>
                  {label}
                </Text>
              </TouchableOpacity>
            );
          })}
        </View>

        {/* ── SOUND ─────────────────────────────────────────────────── */}
        <Text style={[styles.sectionLabel, styles.sectionLabelGap]}>SOUND</Text>
        <View style={styles.lightsRow}>
          {SOUNDS.map(({ key, label }) => (
            <TouchableOpacity
              key={key}
              style={styles.lightBtn}
              onPress={() => playSound(key)}
              activeOpacity={0.7}
            >
              <Text style={styles.lightBtnText}>{label}</Text>
            </TouchableOpacity>
          ))}
        </View>

        {/* spacer — pushes nav bar to bottom */}
        <View style={{ flex: 1 }} />

        {/* ── NAV BAR ───────────────────────────────────────────────── */}
        <View style={styles.navBar}>
          {NAV.map(({ screen, label }) => (
            <TouchableOpacity
              key={screen}
              style={styles.navTab}
              onPress={() => navigation.navigate(screen, { ip })}
              activeOpacity={0.6}
            >
              <Text style={styles.navTabText}>{label}</Text>
            </TouchableOpacity>
          ))}
        </View>
      </View>

      <CruiseModal
        visible={cruiseModalOpen}
        currentUs={cruiseUs}
        onPick={handlePickCruise}
        onCancel={() => setCruise(false)}
      />
    </Screen>
  );
}

// ── Readout component ─────────────────────────────────────────────────────────
function Readout({
  label, value, unit, color, sub,
}: {
  label: string;
  value: string;
  unit?: string;
  color?: string;
  sub?: string;
}) {
  return (
    <View style={styles.readout}>
      <Text style={styles.readoutLabel}>{label}</Text>
      <View style={styles.readoutValueRow}>
        <Text style={[styles.readoutValue, color != null && { color }]}>{value}</Text>
        {unit && (
          <Text style={[styles.readoutUnit, color != null && { color }]}>{unit}</Text>
        )}
      </View>
      <Text style={styles.readoutSub}>{sub ?? ' '}</Text>
    </View>
  );
}

// ── Styles ─────────────────────────────────────────────────────────────────────
const styles = StyleSheet.create({
  inner: { flex: 1, paddingHorizontal: 16, paddingTop: 20 },

  // Header
  header:      { flexDirection: 'row', alignItems: 'flex-start', justifyContent: 'space-between' },
  brandCol:    { flex: 1 },
  brand:       { color: Colors.textPrimary, fontSize: 18, fontWeight: '800', letterSpacing: 4, fontFamily: 'monospace' },
  stripe:      { flexDirection: 'row', width: 120, height: 4, marginTop: 4, borderRadius: 1, overflow: 'hidden' },
  stripeSeg:   { flex: 1, height: 4 },
  modeChip:    { flexDirection: 'row', alignItems: 'center', borderWidth: 1.5, borderRadius: 4, paddingHorizontal: 10, paddingVertical: 5, minWidth: 100, justifyContent: 'center' },
  modeDot:     { fontSize: 10, marginRight: 6 },
  modeChipText:{ fontSize: 13, fontFamily: 'monospace', fontWeight: '800', letterSpacing: 2 },

  // Status sub-row — generous gaps so it doesn't feel scrunched.
  statusRow:    { flexDirection: 'row', alignItems: 'center', marginTop: 22, marginBottom: 32 },
  statusDot:    { fontSize: 9, marginRight: 4 },
  statusLabel:  { color: Colors.textSecondary, fontSize: 11, fontFamily: 'monospace', letterSpacing: 1, marginRight: 6 },
  statusSep:    { color: Colors.textSecondary, fontSize: 11, marginRight: 6, opacity: 0.5 },
  stateSentence:{ fontSize: 11, fontFamily: 'monospace', letterSpacing: 1, fontWeight: '600', flex: 1 },

  // Alarm
  alarm:     { backgroundColor: Colors.danger, padding: 10, borderRadius: 4, marginBottom: 12 },
  alarmText: { color: '#fff', fontWeight: 'bold', textAlign: 'center', fontFamily: 'monospace', letterSpacing: 1, fontSize: 12 },

  // Readouts
  readoutRow:      { flexDirection: 'row', justifyContent: 'space-between', marginBottom: 28 },
  readout:         { alignItems: 'center', flex: 1 },
  readoutLabel:    { color: Colors.textSecondary, fontSize: 10, letterSpacing: 2, fontFamily: 'monospace' },
  readoutValueRow: { flexDirection: 'row', alignItems: 'baseline', marginTop: 4 },
  readoutValue:    { color: Colors.textPrimary, fontSize: 24, fontWeight: '800', fontFamily: 'monospace', letterSpacing: -0.5 },
  readoutUnit:     { color: Colors.textPrimary, fontSize: 12, fontWeight: '600', fontFamily: 'monospace', marginLeft: 2, letterSpacing: 1 },
  readoutSub:      { color: Colors.textSecondary, fontSize: 10, fontFamily: 'monospace', marginTop: 2, minHeight: 12 },

  // Section label (shared by AUTOPILOT, LIGHTS, SOUND)
  sectionLabel:    { color: Colors.textSecondary, fontSize: 10, letterSpacing: 3, fontFamily: 'monospace', fontWeight: '700', marginBottom: 8 },
  sectionLabelGap: { marginTop: 18 },

  // Autopilot — no left-edge bar; chevron is the affordance.
  autopilotRow: { flexDirection: 'row', gap: 10, marginBottom: 20 },
  apCard:       { flex: 1, backgroundColor: Colors.surface, borderRadius: 4, padding: 14 },
  apCardHead:   { flexDirection: 'row', justifyContent: 'space-between', alignItems: 'center' },
  apCardLabel:  { color: Colors.textSecondary, fontSize: 10, letterSpacing: 2, fontFamily: 'monospace' },
  apCardChevron:{ color: Colors.accent, fontSize: 22, lineHeight: 22, fontWeight: '700' },
  apCardValue:  { color: Colors.accent, fontSize: 22, fontWeight: '800', fontFamily: 'monospace', marginTop: 6 },
  apCardUnit:   { fontSize: 12, fontWeight: '600', letterSpacing: 1 },
  apCardSub:    { color: Colors.textSecondary, fontSize: 10, fontFamily: 'monospace', marginTop: 4, letterSpacing: 1 },

  // Lights
  lightsRow:      { flexDirection: 'row', gap: 10, marginBottom: 8 },
  lightBtn:       { flex: 1, backgroundColor: Colors.surface, paddingVertical: 16, borderRadius: 4, alignItems: 'center', borderWidth: 1, borderColor: Colors.surfaceLight },
  lightBtnOn:     { backgroundColor: Colors.accent, borderColor: Colors.accent },
  lightBtnText:   { color: Colors.textSecondary, fontWeight: '800', fontSize: 12, letterSpacing: 2, fontFamily: 'monospace' },
  lightBtnTextOn: { color: '#000' },

  // Bilge status card — compact one-liner on Helm. Full controls live
  // on SystemsScreen. Card goes red when any zone wet.
  bilgeCard:           { backgroundColor: Colors.surface, borderRadius: 4, padding: 12, marginTop: 8, flexDirection: 'row', alignItems: 'baseline', justifyContent: 'space-between' },
  bilgeCardLabel:      { color: Colors.textSecondary, fontSize: 10, letterSpacing: 2, fontFamily: 'monospace', fontWeight: '800' },
  bilgeCardLabelWet:   { color: Colors.danger },
  bilgeCardValue:      { color: Colors.textPrimary, fontSize: 13, letterSpacing: 1, fontFamily: 'monospace', fontWeight: '700' },
  bilgeCardValueWet:   { color: Colors.danger },

  // Nav bar (bottom)
  navBar:      { flexDirection: 'row', borderTopWidth: 1, borderTopColor: Colors.surface, paddingTop: 10, paddingBottom: 6 },
  navTab:      { flex: 1, alignItems: 'center', paddingVertical: 10 },
  navTabText:  { color: Colors.accent, fontSize: 12, fontWeight: '700', letterSpacing: 3, fontFamily: 'monospace' },
});
