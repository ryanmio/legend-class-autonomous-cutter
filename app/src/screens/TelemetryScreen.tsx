// TelemetryScreen.tsx
// Live readouts: battery voltage/current, GPS, IMU heading/roll/pitch, depth,
// water sensor status, WiFi signal, heap, uptime.

import React from 'react';
import { View, Text, ScrollView, StyleSheet } from 'react-native';
import { NativeStackScreenProps } from '@react-navigation/native-stack';
import { RootStackParamList } from '../../App';
import { Colors } from '../constants';
import { useTelemetry } from '../hooks/useTelemetry';
import EmergencyStop from '../components/EmergencyStop';

type Props = NativeStackScreenProps<RootStackParamList, 'Telemetry'>;

export default function TelemetryScreen({ route }: Props) {
  const { ip } = route.params;
  const { data, connected } = useTelemetry();

  const rows: [string, string | number | boolean | undefined][] = data ? [
    ['Firmware',    data.v],
    ['Uptime',      `${data.uptime} s`],
    ['Free Heap',   `${data.heap} B`],
    ['Battery',     `${data.batt_v} V  /  ${data.batt_a} A`],
    ['Batt Warn',   data.batt_low ? '⚠ LOW' : 'OK'],
    ['GPS Fix',     data.gps_fix ? `✓ (${data.sats} sats)` : '✗'],
    ['Position',    `${data.lat}, ${data.lon}`],
    ['Speed',       `${data.speed_kts} kts`],
    ['Course',      `${data.course}°`],
    ['Heading',     `${data.heading}°`],
    ['Roll',        `${data.roll}°`],
    ['Pitch',       `${data.pitch}°`],
    ['Depth',       data.sonar_ok ? `${data.depth_m} m` : 'No echo'],
    ['Bilge Fwd',   data.bilge_fwd ? '⚠ WET' : 'dry'],
    ['Bilge Aft',   data.bilge_aft ? '⚠ WET' : 'dry'],
    ['Pump',        data.pump ? 'RUNNING' : 'off'],
  ] : [];

  return (
    <View style={styles.container}>
      <Text style={styles.title}>TELEMETRY  <Text style={{ color: connected ? Colors.success : Colors.danger }}>●</Text></Text>
      <ScrollView>
        {rows.map(([label, val]) => (
          <View key={label} style={styles.row}>
            <Text style={styles.label}>{label}</Text>
            <Text style={styles.value}>{String(val)}</Text>
          </View>
        ))}
        {!data && <Text style={styles.empty}>Waiting for telemetry…</Text>}
      </ScrollView>
      <EmergencyStop ip={ip} />
    </View>
  );
}

const styles = StyleSheet.create({
  container: { flex: 1, backgroundColor: Colors.background, padding: 16 },
  title:     { color: Colors.accent, fontSize: 18, fontWeight: 'bold', marginBottom: 12 },
  row:       { flexDirection: 'row', justifyContent: 'space-between', paddingVertical: 6, borderBottomColor: Colors.surface, borderBottomWidth: 1 },
  label:     { color: Colors.textSecondary, fontSize: 13 },
  value:     { color: Colors.textPrimary, fontSize: 13, fontWeight: '600' },
  empty:     { color: Colors.textSecondary, textAlign: 'center', marginTop: 40, fontStyle: 'italic' },
});
