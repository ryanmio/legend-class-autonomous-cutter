import React, { useCallback, useState } from 'react';
import { View, Text, TextInput, TouchableOpacity, ScrollView, StyleSheet } from 'react-native';
import { NativeStackScreenProps } from '@react-navigation/native-stack';
import * as Haptics from 'expo-haptics';
import { RootStackParamList } from '../../App';
import { Colors } from '../constants';
import { useTelemetry } from '../hooks/useTelemetry';
import { postBilge, setRadar, setDepth } from '../services/esp32Service';
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
        <WeaponsSection />
        <SurveySection />
        <SettingsSection />
      </ScrollView>
    </Screen>
  );
}

// ── BILGE ──────────────────────────────────────────────────────────────────────
// Damage-control panel: side-profile hull silhouette with three vertical
// thirds that fill red when their bilge sensor is wet. PUMP button below
// drives /bilge {on:bool}; auto-pump on any wet sensor is firmware-side.
function BilgeSection({ ip }: { ip: string }) {
  const { data } = useTelemetry();

  const togglePump = useCallback(() => {
    Haptics.impactAsync(Haptics.ImpactFeedbackStyle.Medium);
    postBilge(ip, !(data?.pump_manual ?? false)).catch(() => {});
  }, [ip, data?.pump_manual]);

  return (
    <View style={styles.section}>
      <SectionHeader label="BILGE" />

      <BoatDamagePanel
        fwdWet={!!data?.bilge_fwd}
        midWet={!!data?.bilge_mid}
        rearWet={!!data?.bilge_rear}
      />

      <TouchableOpacity
        style={[styles.pumpBtn, data?.pump && styles.pumpBtnOn]}
        onPress={togglePump}
        activeOpacity={0.7}
      >
        <Text style={[styles.pumpBtnText, data?.pump && styles.pumpBtnTextOn]}>
          PUMP {data?.pump ? 'ON' : 'OFF'}
          {data?.pump_manual ? ' · MANUAL' : ''}
        </Text>
      </TouchableOpacity>
      <Text style={styles.pumpSub}>
        Auto fires on any wet sensor (latches off after 60 s continuous run — sensor stuck?). Manual override stays on for 60 s.
      </Text>
      {data?.pump_stuck && (
        <Text style={[styles.pumpSub, styles.pumpSubAlarm]}>
          ⚠ AUTO-PUMP LATCHED OFF — sensor reading wet for &gt;60 s. Use manual PUMP to override; check probe.
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
// PLACEHOLDER — controls disabled.
// PID save and IMU calibration will be wired when production firmware
// exposes durable versions of /pid (NVS-persisted) and /calibrate-imu.
// Today /pid is live-tunable on test_29 but values don't survive reboot,
// and /calibrate-imu doesn't exist.
function SettingsSection() {
  const [kp, setKp] = useState('3.0');
  const [ki, setKi] = useState('0.0');
  const [kd, setKd] = useState('8.0');

  return (
    <View style={styles.section}>
      <SectionHeader label="SETTINGS" />
      <View style={styles.banner}>
        <Text style={styles.bannerText}>PLACEHOLDER — CONTROLS DISABLED</Text>
      </View>

      <Text style={styles.subSection}>HEADING-HOLD PID</Text>
      {([['Kp', kp, setKp], ['Ki', ki, setKi], ['Kd', kd, setKd]] as const).map(([label, val, setter]) => (
        <View key={label} style={styles.inputRow}>
          <Text style={styles.inputLabel}>{label}</Text>
          <TextInput
            style={styles.input}
            value={val}
            onChangeText={setter as (v: string) => void}
            keyboardType="decimal-pad"
            editable={false}
          />
        </View>
      ))}
      <TouchableOpacity style={[styles.btn, styles.btnDisabled]} disabled>
        <Text style={styles.btnText}>SAVE PID TO ESP32</Text>
      </TouchableOpacity>

      <Text style={styles.subSection}>SENSORS</Text>
      <TouchableOpacity style={[styles.btn, styles.btnDisabled]} disabled>
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

  banner:      { backgroundColor: Colors.surface, borderLeftWidth: 3, borderLeftColor: Colors.warning, padding: 10, borderRadius: 2, marginBottom: 12 },
  bannerText:  { color: Colors.warning, fontSize: 10, letterSpacing: 2, fontFamily: 'monospace', fontWeight: '700' },

  inputRow:    { flexDirection: 'row', alignItems: 'center', marginBottom: 8 },
  inputLabel:  { color: Colors.textSecondary, width: 32, fontSize: 14, fontFamily: 'monospace' },
  input:       { flex: 1, backgroundColor: Colors.surface, color: Colors.textSecondary, padding: 10, borderRadius: 4, fontSize: 15, fontFamily: 'monospace' },

  btn:         { backgroundColor: Colors.surfaceLight, padding: 14, borderRadius: 4, alignItems: 'center', marginTop: 8 },
  btnDisabled: { opacity: 0.4 },
  btnText:     { color: Colors.accent, fontWeight: '800', fontSize: 12, letterSpacing: 2, fontFamily: 'monospace' },

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
