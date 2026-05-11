import React, { useState } from 'react';
import { View, Text, TextInput, TouchableOpacity, ScrollView, StyleSheet } from 'react-native';
import { NativeStackScreenProps } from '@react-navigation/native-stack';
import { RootStackParamList } from '../../App';
import { Colors } from '../constants';
import Screen from '../components/Screen';

type Props = NativeStackScreenProps<RootStackParamList, 'Systems'>;

// Consolidated systems page. WEAPONS, SURVEY, SETTINGS as sections in a
// single scroll. All three are placeholders today — see comments in each
// section for the gate that has to land before controls go live. When
// any section grows substantial, it can be broken out into its own page.
export default function SystemsScreen(_: Props) {
  return (
    <Screen>
      <ScrollView contentContainerStyle={styles.scroll}>
        <Text style={styles.title}>SYSTEMS</Text>

        <WeaponsSection />
        <SurveySection />
        <SettingsSection />
      </ScrollView>
    </Screen>
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
});
