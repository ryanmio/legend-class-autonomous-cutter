import React, { useCallback } from 'react';
import { View, Text, TouchableOpacity, ScrollView, StyleSheet } from 'react-native';
import { NativeStackScreenProps, NativeStackNavigationProp } from '@react-navigation/native-stack';
import { useNavigation } from '@react-navigation/native';
import * as Haptics from 'expo-haptics';
import { RootStackParamList } from '../../App';
import { Colors } from '../constants';
import { useTelemetry } from '../hooks/useTelemetry';
import { postBilge, setRadar, setDepth, setPID } from '../services/esp32Service';
import Screen from '../components/Screen';
import BoatDamagePanel from '../components/BoatDamagePanel';

type Props = NativeStackScreenProps<RootStackParamList, 'Systems'>;

// Consolidated systems page. BILGE is live; WEAPONS / SURVEY / SETTINGS
// are placeholders today — see comments in each section for the gate
// that has to land before controls go live. When any section grows
// substantial, it can be broken out into its own page.
export default function SystemsScreen({ route }: Props) {
  const { ip } = route.params;
  return (
    <Screen>
      <ScrollView contentContainerStyle={styles.scroll}>
        <Text style={styles.title}>SYSTEMS</Text>

        <BilgeSection ip={ip} />
        <RadarSection ip={ip} />
        <DepthSection ip={ip} />
        <FlightsSection />
        <WeaponsSection />
        <SurveySection />
        <SettingsSection ip={ip} />
      </ScrollView>
    </Screen>
  );
}

// ── FLIGHT LOGS ────────────────────────────────────────────────────────────────
// Auto-logger captures each run as a saved flight in AsyncStorage.
// This button opens the list — see telemetryLogger.ts + FLIGHT_LOG_PLAN.md.
function FlightsSection() {
  const navigation = useNavigation<NativeStackNavigationProp<RootStackParamList>>();
  return (
    <View style={styles.section}>
      <SectionHeader label="FLIGHT LOGS" />
      <TouchableOpacity style={styles.btn} onPress={() => navigation.navigate('Flights')}>
        <Text style={styles.btnText}>OPEN FLIGHT LOGS</Text>
      </TouchableOpacity>
    </View>
  );
}

// ── BILGE ──────────────────────────────────────────────────────────────────────
// Damage-control panel: side-profile hull silhouette with three vertical
// thirds that fill red when their bilge sensor is wet. Pump lives in the
// rear compartment and duty-cycles 6 s on / 6 s off. Auto fires on
// bilge_rear wet, caps at 60 s of cycling; manual cycles forever until
// the operator taps PUMP again to stop.
function BilgeSection({ ip }: { ip: string }) {
  const { data } = useTelemetry();
  const phase    = data?.pump_phase ?? 'off';
  const cycling  = phase !== 'off';
  const manual   = !!data?.pump_manual;
  const cycle    = data?.pump_cycle;

  const togglePump = useCallback(() => {
    Haptics.impactAsync(Haptics.ImpactFeedbackStyle.Medium);
    // Manual-on if not already cycling under manual; manual-off otherwise.
    // (Stopping during an AUTO cycle is intentionally not exposed —
    // operator-initiated stop only applies to manual.)
    postBilge(ip, !manual).catch(() => {});
  }, [ip, manual]);

  // Button label reflects current phase + cycle. Tapping always toggles
  // manual: starts a fresh 6/6 sequence, or stops one in progress.
  const phaseLabel =
    phase === 'on'    ? `ON (cycle ${cycle ?? '?'})`
  : phase === 'pause' ? `PAUSE (cycle ${cycle ?? '?'})`
  :                     'OFF';
  const sourceLabel = manual ? ' · MANUAL' : (cycling ? ' · AUTO' : '');

  return (
    <View style={styles.section}>
      <SectionHeader label="BILGE" />

      <BoatDamagePanel
        fwdWet={!!data?.bilge_fwd}
        midWet={!!data?.bilge_mid}
        rearWet={!!data?.bilge_rear}
      />

      <TouchableOpacity
        style={[styles.pumpBtn, !!data?.pump && styles.pumpBtnOn]}
        onPress={togglePump}
        activeOpacity={0.7}
      >
        <Text style={[styles.pumpBtnText, !!data?.pump && styles.pumpBtnTextOn]}>
          PUMP {phaseLabel}{sourceLabel}
        </Text>
      </TouchableOpacity>
      <Text style={styles.pumpSub}>
        Pump cycles 6 s ON / 6 s OFF. Auto engages on rear sensor wet and gives up after 60 s. Manual cycles until you tap PUMP again to stop.
      </Text>
      {data?.pump_stuck && (
        <Text style={[styles.pumpSub, styles.pumpSubAlarm]}>
          ⚠ AUTO PUMP GAVE UP — rear sensor still wet after 60 s of cycling. Tap PUMP to engage manual override.
        </Text>
      )}
    </View>
  );
}

// ── RADAR ──────────────────────────────────────────────────────────────────────
// Mast-top TRS-3D radar dish. 3 V planetary gear motor on GPIO 2,
// firmware-side burst-PWM (smooth mode removed — produced propeller-
// speed spin at any duty). Each torque preset is a tested or
// extrapolated {speed, burst_ms, pause_ms} tuple. 25% is the
// empirically-tuned good setting (2026-05-20, ~36° step at ~5 steps/sec).
// Higher torque presets give the motor more authority (overcome
// stiction better) at the cost of slightly faster apparent rotation.
type RadarPreset = { label: string; on: boolean; speed?: number; burst_ms?: number; pause_ms?: number };
const RADAR_PRESETS: RadarPreset[] = [
  { label: 'OFF',  on: false },
  { label: '25%',  on: true, speed: 25,  burst_ms: 3, pause_ms: 200 },  // TESTED GOOD
  { label: '50%',  on: true, speed: 50,  burst_ms: 2, pause_ms: 200 },
  { label: '75%',  on: true, speed: 75,  burst_ms: 2, pause_ms: 200 },
  { label: '100%', on: true, speed: 100, burst_ms: 2, pause_ms: 200 },
];

function RadarSection({ ip }: { ip: string }) {
  const { data } = useTelemetry();
  const on    = !!data?.radar_on;
  const speed = data?.radar_speed ?? 0;
  // The "active" preset is OFF when off, otherwise whichever preset's
  // speed matches firmware state. Burst/pause params aren't compared —
  // they're locked to the preset, and any drift means the operator
  // hand-tweaked via curl (not exposed in the UI).
  const activeSpeed = on ? speed : 0;

  const pickPreset = useCallback((preset: RadarPreset) => {
    Haptics.impactAsync(Haptics.ImpactFeedbackStyle.Light);
    setRadar(ip, preset.on
      ? { on: true, speed: preset.speed, burst_ms: preset.burst_ms, pause_ms: preset.pause_ms }
      : { on: false }
    ).catch(() => {});
  }, [ip]);

  return (
    <View style={styles.section}>
      <SectionHeader label="RADAR" />
      <View style={styles.radarRow}>
        {RADAR_PRESETS.map((p) => {
          const active = p.on ? (p.speed === activeSpeed) : (activeSpeed === 0);
          return (
            <TouchableOpacity
              key={p.label}
              style={[styles.radarBtn, active && styles.radarBtnActive]}
              onPress={() => pickPreset(p)}
              activeOpacity={0.7}
            >
              <Text style={[styles.radarBtnText, active && styles.radarBtnTextActive]}>
                {p.label}
              </Text>
            </TouchableOpacity>
          );
        })}
      </View>
      <Text style={styles.pumpSub}>
        Higher % = more torque + slightly faster sweep.
      </Text>
    </View>
  );
}

// ── DEPTH ──────────────────────────────────────────────────────────────────────
// Bottom-facing RCWL-1655 sonar. RUN polls every 20 s, CHECK fires
// a single ping, STOP halts + clears the last reading. The last
// reading persists in firmware across CHECK→RUN transitions; only
// STOP wipes it.
function DepthSection({ ip }: { ip: string }) {
  const { data } = useTelemetry();
  const depthStr = data?.depth_m;
  const mode     = data?.depth_mode ?? 'off';
  const ageMs    = data?.depth_age_ms;

  const tap = useCallback((m: 'stop' | 'check' | 'run') => {
    Haptics.impactAsync(Haptics.ImpactFeedbackStyle.Light);
    setDepth(ip, m).catch(() => {});
  }, [ip]);

  const ageLabel =
    depthStr == null      ? 'no reading' :
    ageMs == null         ? '' :
    ageMs < 2000          ? 'just now' :
    ageMs < 60000         ? `${Math.round(ageMs / 1000)} s ago` :
                            `${Math.round(ageMs / 60000)} min ago`;

  return (
    <View style={styles.section}>
      <SectionHeader label="DEPTH" />
      <View style={styles.depthReadoutBlock}>
        <Text style={styles.depthValue}>
          {depthStr != null ? depthStr : '--'}
          <Text style={styles.depthUnit}>{depthStr != null ? ' M' : ''}</Text>
        </Text>
        <Text style={styles.depthMeta}>
          {mode === 'run' ? 'POLLING · ' : ''}{ageLabel}
        </Text>
      </View>
      <View style={styles.depthBtnRow}>
        <TouchableOpacity
          style={[styles.depthBtn, mode === 'off' && depthStr == null && styles.depthBtnActive]}
          onPress={() => tap('stop')}
          activeOpacity={0.7}
        >
          <Text style={styles.depthBtnText}>STOP</Text>
        </TouchableOpacity>
        <TouchableOpacity
          style={styles.depthBtn}
          onPress={() => tap('check')}
          activeOpacity={0.7}
        >
          <Text style={styles.depthBtnText}>CHECK</Text>
        </TouchableOpacity>
        <TouchableOpacity
          style={[styles.depthBtn, mode === 'run' && styles.depthBtnActive]}
          onPress={() => tap('run')}
          activeOpacity={0.7}
        >
          <Text style={styles.depthBtnText}>RUN</Text>
        </TouchableOpacity>
      </View>
    </View>
  );
}

// ── WEAPONS ────────────────────────────────────────────────────────────────────
// Deck gun + CIWS + audio controls land here once production firmware
// exposes /gun, /ciws, /anim, /audio endpoints. Intentionally empty until
// then so we don't ship dead buttons.
function WeaponsSection() {
  return (
    <View style={styles.section}>
      <SectionHeader label="WEAPONS" />
      <Text style={styles.placeholder}>Controls land when firmware is ready.</Text>
    </View>
  );
}

// ── SURVEY ─────────────────────────────────────────────────────────────────────
// Bounding-box editor, grid generator, bathymetric/depth logging land here
// once sonar hardware is wired and a depth feed exists in /telemetry.
function SurveySection() {
  return (
    <View style={styles.section}>
      <SectionHeader label="SURVEY" />
      <Text style={styles.placeholder}>Survey tooling lands when sonar is wired.</Text>
    </View>
  );
}

// ── SETTINGS ───────────────────────────────────────────────────────────────────
// Heading-hold kd presets (v0.8.0+ firmware). Values are RAM-only on the
// boat: a reboot restores the flashed config.h defaults (kd 2.0 / diff 1.0
// since v0.8.1), so the boat never depends on the app. kp is fixed at 1.5;
// each preset pairs kd with a diff_gain holding kd×diff_gain ≈ 2.0, keeping
// the diff-thrust crossing damping constant so a sweep varies only the
// rudder D term. LEGACY is the pre-v0.8.1 tune, kept as an escape hatch.
// Active preset is derived from live telemetry (the boat is source of truth).
type PidPreset = { kd: number; diffGain: number; hint: string };
const PID_KP = 1.5;
const PID_PRESETS: PidPreset[] = [
  { kd: 2.0, diffGain: 1.0,  hint: 'DEFAULT'  },
  { kd: 3.0, diffGain: 0.65, hint: 'FIRMER'   },
  { kd: 1.2, diffGain: 1.65, hint: 'SNAPPIER' },
  { kd: 8.0, diffGain: 0.25, hint: 'LEGACY'   },
];

function SettingsSection({ ip }: { ip: string }) {
  const navigation = useNavigation<NativeStackNavigationProp<RootStackParamList>>();
  const { data } = useTelemetry();
  const liveKd   = data?.pid_kd    != null ? parseFloat(data.pid_kd)    : null;
  const liveDiff = data?.diff_gain != null ? parseFloat(data.diff_gain) : null;

  const pickPreset = useCallback((p: PidPreset) => {
    Haptics.impactAsync(Haptics.ImpactFeedbackStyle.Medium);
    setPID(ip, { kp: PID_KP, kd: p.kd, diff_gain: p.diffGain }).catch(() => {});
  }, [ip]);

  return (
    <View style={styles.section}>
      <SectionHeader label="SETTINGS" />

      <Text style={styles.subSection}>HEADING-HOLD KD PRESETS</Text>
      <View style={styles.pidRow}>
        {PID_PRESETS.map((p) => {
          const active =
            liveKd != null && Math.abs(liveKd - p.kd) < 0.005 &&
            liveDiff != null && Math.abs(liveDiff - p.diffGain) < 0.005;
          return (
            <TouchableOpacity
              key={p.kd}
              style={[styles.pidBtn, active && styles.pidBtnActive]}
              onPress={() => pickPreset(p)}
              activeOpacity={0.7}
            >
              <Text style={[styles.pidBtnKd, active && styles.pidBtnTextActive]}>{p.kd.toFixed(1)}</Text>
              <Text style={[styles.pidBtnHint, active && styles.pidBtnTextActive]}>{p.hint}</Text>
            </TouchableOpacity>
          );
        })}
      </View>
      <Text style={styles.pumpSub}>
        {`kp ${data?.pid_kp ?? '--'} · kd ${data?.pid_kd ?? '--'} · diff ${data?.diff_gain ?? '--'} — RAM only; reboot restores flashed defaults.`}
      </Text>

      <Text style={styles.subSection}>SENSORS</Text>
      <TouchableOpacity style={styles.btn} onPress={() => navigation.navigate('Calibration', { ip })}>
        <Text style={styles.btnText}>CALIBRATE IMU COMPASS</Text>
      </TouchableOpacity>
    </View>
  );
}

// ── Shared header divider ─────────────────────────────────────────────────────
function SectionHeader({ label }: { label: string }) {
  return (
    <View style={styles.sectionHeader}>
      <View style={styles.sectionHeaderRule} />
      <Text style={styles.sectionHeaderText}>{label}</Text>
      <View style={styles.sectionHeaderRule} />
    </View>
  );
}

// ── Styles ─────────────────────────────────────────────────────────────────────
const styles = StyleSheet.create({
  scroll: { padding: 16, paddingBottom: 32 },
  title:  { color: Colors.textPrimary, fontSize: 18, fontWeight: '800', letterSpacing: 4, fontFamily: 'monospace', marginBottom: 24 },

  section: { marginBottom: 28 },

  sectionHeader:     { flexDirection: 'row', alignItems: 'center', marginBottom: 12 },
  sectionHeaderRule: { flex: 1, height: 1, backgroundColor: Colors.surface },
  sectionHeaderText: { color: Colors.accent, fontSize: 11, letterSpacing: 4, fontFamily: 'monospace', fontWeight: '800', marginHorizontal: 12 },

  subSection:  { color: Colors.textSecondary, fontSize: 10, letterSpacing: 2, marginTop: 16, marginBottom: 8, fontFamily: 'monospace' },
  placeholder: { color: Colors.textSecondary, fontSize: 12, fontStyle: 'italic', textAlign: 'center', paddingVertical: 8 },

  btn:         { backgroundColor: Colors.surfaceLight, padding: 14, borderRadius: 4, alignItems: 'center', marginTop: 8 },
  btnText:     { color: Colors.accent, fontWeight: '800', fontSize: 12, letterSpacing: 2, fontFamily: 'monospace' },

  // PID kd preset buttons — 4 across, the active one inverts to accent (see RADAR).
  pidRow:           { flexDirection: 'row', gap: 6 },
  pidBtn:           { flex: 1, backgroundColor: Colors.surface, borderRadius: 4, paddingVertical: 10, alignItems: 'center', borderWidth: 1, borderColor: Colors.surfaceLight },
  pidBtnActive:     { backgroundColor: Colors.accent, borderColor: Colors.accent },
  pidBtnKd:         { color: Colors.textSecondary, fontWeight: '800', fontSize: 16, fontFamily: 'monospace' },
  pidBtnHint:       { color: Colors.textSecondary, fontWeight: '700', fontSize: 8, letterSpacing: 1, fontFamily: 'monospace', marginTop: 2 },
  pidBtnTextActive: { color: '#000' },

  pumpBtn:        { backgroundColor: Colors.surfaceLight, padding: 14, borderRadius: 4, alignItems: 'center' },
  pumpBtnOn:      { backgroundColor: Colors.success, borderColor: Colors.success },
  pumpBtnText:    { color: Colors.accent, fontWeight: '800', fontSize: 13, letterSpacing: 2, fontFamily: 'monospace' },
  pumpBtnTextOn:  { color: '#000' },
  pumpSub:        { color: Colors.textSecondary, fontSize: 10, letterSpacing: 1, fontFamily: 'monospace', marginTop: 8, fontStyle: 'italic' },
  pumpSubAlarm:   { color: Colors.danger, fontWeight: '700' },

  // Depth readout + STOP/CHECK/RUN row.
  depthReadoutBlock:  { backgroundColor: Colors.surface, borderRadius: 4, padding: 14, marginBottom: 8 },
  depthValue:         { color: Colors.accent, fontSize: 28, fontWeight: '800', fontFamily: 'monospace', letterSpacing: 1 },
  depthUnit:          { fontSize: 14, fontWeight: '600', letterSpacing: 1 },
  depthMeta:          { color: Colors.textSecondary, fontSize: 10, letterSpacing: 1, fontFamily: 'monospace', marginTop: 4 },
  depthBtnRow:        { flexDirection: 'row', gap: 6 },
  depthBtn:           { flex: 1, backgroundColor: Colors.surface, borderRadius: 4, paddingVertical: 14, alignItems: 'center', borderWidth: 1, borderColor: Colors.surfaceLight },
  depthBtnActive:     { backgroundColor: Colors.accent, borderColor: Colors.accent },
  depthBtnText:       { color: Colors.textSecondary, fontWeight: '800', fontSize: 12, letterSpacing: 2, fontFamily: 'monospace' },

  // Radar preset buttons — 5 across, the active one inverts to accent.
  radarRow:           { flexDirection: 'row', gap: 6 },
  radarBtn:           { flex: 1, backgroundColor: Colors.surface, borderRadius: 4, paddingVertical: 14, alignItems: 'center', borderWidth: 1, borderColor: Colors.surfaceLight },
  radarBtnActive:     { backgroundColor: Colors.accent, borderColor: Colors.accent },
  radarBtnText:       { color: Colors.textSecondary, fontWeight: '800', fontSize: 11, letterSpacing: 1, fontFamily: 'monospace' },
  radarBtnTextActive: { color: '#000' },
});
