import React, { useState } from 'react';
import { View, Text, TextInput, TouchableOpacity, StyleSheet, ScrollView } from 'react-native';
import { NativeStackScreenProps } from '@react-navigation/native-stack';
import { RootStackParamList } from '../../App';
import { Colors } from '../constants';
import Screen from '../components/Screen';

type Props = NativeStackScreenProps<RootStackParamList, 'Settings'>;

// PLACEHOLDER SCREEN.
// Visual scaffolding only — PID save, IMU calibration, and servo limit
// tuning will be wired here once production firmware exposes durable
// versions of /pid (persist to NVS) and /calibrate-imu. Today /pid is
// live-tunable from test_29 but is not persisted, and /calibrate-imu
// does not exist. Buttons are intentionally disabled so we don't ship
// non-working controls.
export default function SettingsScreen(_: Props) {
  const [kp, setKp] = useState('3.0');
  const [ki, setKi] = useState('0.0');
  const [kd, setKd] = useState('8.0');

  return (
    <Screen>
      <ScrollView contentContainerStyle={styles.scroll}>
        <Text style={styles.title}>SETTINGS</Text>

        <View style={styles.banner}>
          <Text style={styles.bannerText}>PLACEHOLDER — CONTROLS DISABLED</Text>
        </View>

        <Text style={styles.section}>HEADING-HOLD PID</Text>
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

        <Text style={styles.section}>SENSORS</Text>
        <TouchableOpacity style={[styles.btn, styles.btnDisabled]} disabled>
          <Text style={styles.btnText}>CALIBRATE IMU COMPASS</Text>
        </TouchableOpacity>
      </ScrollView>
    </Screen>
  );
}

const styles = StyleSheet.create({
  scroll:      { padding: 16, paddingBottom: 32 },
  title:       { color: Colors.accent, fontSize: 18, fontWeight: 'bold', marginBottom: 16 },
  banner:      { backgroundColor: Colors.surface, borderLeftWidth: 3, borderLeftColor: Colors.warning, padding: 10, borderRadius: 4, marginBottom: 16 },
  bannerText:  { color: Colors.warning, fontSize: 11, letterSpacing: 2, fontFamily: 'monospace' },
  section:     { color: Colors.textSecondary, fontSize: 11, letterSpacing: 2, marginTop: 24, marginBottom: 10 },
  inputRow:    { flexDirection: 'row', alignItems: 'center', marginBottom: 8 },
  inputLabel:  { color: Colors.textSecondary, width: 32, fontSize: 14 },
  input:       { flex: 1, backgroundColor: Colors.surface, color: Colors.textSecondary, padding: 10, borderRadius: 6, fontSize: 15 },
  btn:         { backgroundColor: Colors.surfaceLight, padding: 14, borderRadius: 8, alignItems: 'center', marginTop: 8 },
  btnDisabled: { opacity: 0.4 },
  btnText:     { color: Colors.accent, fontWeight: 'bold', fontSize: 13, letterSpacing: 1 },
});
