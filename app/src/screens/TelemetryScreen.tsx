// TelemetryScreen.tsx
// Live readouts — only renders rows for fields actually present in the
// current firmware's telemetry broadcast. Works with any test sketch.

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

  // Build row list dynamically — skip any field the firmware didn't send.
  // Tuple: [label, display string, warn flag]
  const rows: [string, string, boolean?][] = [];

  if (data) {
    rows.push(['Firmware', data.v]);
    if (data.uptime  != null) rows.push(['Uptime',     `${data.uptime} s`]);
    if (data.heap    != null) rows.push(['Free Heap',  `${Math.round(data.heap / 1024)} KB`]);
    if (data.mode    != null) rows.push(['Mode',        data.mode]);

    if (data.batt_v  != null) rows.push(['Battery',    `${data.batt_v} V  /  ${data.batt_a ?? '?'} A`, data.batt_low]);
    if (data.batt_low!= null) rows.push(['Batt Warn',  data.batt_low  ? '⚠ LOW' : 'OK',   data.batt_low]);
    if (data.batt_crit!=null) rows.push(['Batt Crit',  data.batt_crit ? '⚠ CRITICAL' : 'OK', data.batt_crit]);

    if (data.gps_fix != null) rows.push(['GPS',        data.gps_fix ? `✓ fix  ${data.sats ?? '?'} sats` : '✗ no fix']);
    if (data.gps_fix && data.lat != null)
                              rows.push(['Position',   `${data.lat}, ${data.lon}`]);
    if (data.speed_kts!=null) rows.push(['Speed',      `${data.speed_kts} kts`]);
    if (data.course  != null) rows.push(['Course',     `${data.course}°`]);
    if (data.heading != null) rows.push(['Heading',    `${data.heading}°`]);
    if (data.roll    != null) rows.push(['Roll',       `${data.roll}°`]);
    if (data.pitch   != null) rows.push(['Pitch',      `${data.pitch}°`]);

    if (data.sonar_ok!= null) rows.push(['Depth',      data.sonar_ok ? `${data.depth_m} m` : 'no echo']);

    if (data.bilge_fwd!=null) rows.push(['Bilge Fwd',  data.bilge_fwd ? '⚠ WET' : 'dry', data.bilge_fwd]);
    if (data.bilge_aft!=null) rows.push(['Bilge Aft',  data.bilge_aft ? '⚠ WET' : 'dry', data.bilge_aft]);
    if (data.pump    != null) rows.push(['Bilge Pump', data.pump      ? 'RUNNING' : 'off']);

    if (data.nav_on    != null) rows.push(['Nav lights',    data.nav_on    ? 'ON' : 'off']);
    if (data.bridge_on != null) rows.push(['Bridge lights', data.bridge_on ? 'ON' : 'off']);
    if (data.deck_on   != null) rows.push(['Deck lights',   data.deck_on   ? 'ON' : 'off']);
  }

  return (
    <View style={styles.container}>
      <Text style={styles.title}>
        TELEMETRY{'  '}
        <Text style={{ color: connected ? Colors.success : Colors.danger }}>●</Text>
      </Text>
      <ScrollView>
        {rows.map(([label, val, warn]) => (
          <View key={label} style={styles.row}>
            <Text style={styles.label}>{label}</Text>
            <Text style={[styles.value, warn && { color: Colors.warning }]}>{val}</Text>
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
