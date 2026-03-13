// SettingsScreen.tsx
// WiFi IP config, PID tuning, servo limit calibration,
// failsafe config, IMU calibration trigger.

import React, { useState } from 'react';
import { View, Text, TextInput, TouchableOpacity, StyleSheet, ScrollView, Alert } from 'react-native';
import { NativeStackScreenProps } from '@react-navigation/native-stack';
import { RootStackParamList } from '../../App';
import { Colors } from '../constants';
import { setPID, triggerIMUCalibration } from '../services/esp32Service';
import EmergencyStop from '../components/EmergencyStop';

type Props = NativeStackScreenProps<RootStackParamList, 'Settings'>;

export default function SettingsScreen({ route }: Props) {
  const { ip } = route.params;
  const [kp, setKp] = useState('2.0');
  const [ki, setKi] = useState('0.05');
  const [kd, setKd] = useState('0.5');

  const handleSavePID = async () => {
    try {
      await setPID(ip, { kp: parseFloat(kp), ki: parseFloat(ki), kd: parseFloat(kd) });
      Alert.alert('PID saved', 'Gains written to ESP32 NVS.');
    } catch {
      Alert.alert('Error', 'Failed to send PID params.');
    }
  };

  const handleCalibrate = async () => {
    Alert.alert(
      'IMU Calibration',
      'The boat will rotate 360° over 30 seconds. Place it on open water first.',
      [
        { text: 'Cancel', style: 'cancel' },
        { text: 'Start', onPress: async () => {
          await triggerIMUCalibration(ip).catch(() => {});
        }},
      ]
    );
  };

  return (
    <ScrollView style={styles.container} contentContainerStyle={{ padding: 16 }}>
      <Text style={styles.title}>SETTINGS</Text>

      <Text style={styles.section}>HEADING-HOLD PID</Text>
      {([['Kp', kp, setKp], ['Ki', ki, setKi], ['Kd', kd, setKd]] as const).map(([label, val, setter]) => (
        <View key={label} style={styles.inputRow}>
          <Text style={styles.inputLabel}>{label}</Text>
          <TextInput
            style={styles.input}
            value={val}
            onChangeText={setter as (v: string) => void}
            keyboardType="decimal-pad"
          />
        </View>
      ))}
      <TouchableOpacity style={styles.btn} onPress={handleSavePID}>
        <Text style={styles.btnText}>SAVE PID TO ESP32</Text>
      </TouchableOpacity>

      <Text style={styles.section}>SENSORS</Text>
      <TouchableOpacity style={styles.btn} onPress={handleCalibrate}>
        <Text style={styles.btnText}>CALIBRATE IMU COMPASS</Text>
      </TouchableOpacity>

      <Text style={styles.placeholder}>Servo limit calibration (Phase 4)</Text>

      <EmergencyStop ip={ip} />
    </ScrollView>
  );
}

const styles = StyleSheet.create({
  container:   { flex: 1, backgroundColor: Colors.background },
  title:       { color: Colors.accent, fontSize: 18, fontWeight: 'bold', marginBottom: 16 },
  section:     { color: Colors.textSecondary, fontSize: 11, letterSpacing: 2, marginTop: 24, marginBottom: 10 },
  inputRow:    { flexDirection: 'row', alignItems: 'center', marginBottom: 8 },
  inputLabel:  { color: Colors.textSecondary, width: 32, fontSize: 14 },
  input:       { flex: 1, backgroundColor: Colors.surface, color: Colors.textPrimary, padding: 10, borderRadius: 6, fontSize: 15 },
  btn:         { backgroundColor: Colors.surfaceLight, padding: 14, borderRadius: 8, alignItems: 'center', marginTop: 8 },
  btnText:     { color: Colors.accent, fontWeight: 'bold', fontSize: 13, letterSpacing: 1 },
  placeholder: { color: Colors.textSecondary, fontStyle: 'italic', marginTop: 24 },
});
