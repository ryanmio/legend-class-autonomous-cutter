// HelmScreen.tsx
// Primary sailing screen: throttle slider, rudder control, speed/heading readout,
// battery indicator, bilge alarm, mode indicator, navigation to other screens.

import React from 'react';
import { View, Text, StyleSheet, TouchableOpacity } from 'react-native';
import { NativeStackScreenProps } from '@react-navigation/native-stack';
import { useKeepAwake } from 'expo-keep-awake';
import { RootStackParamList } from '../../App';
import { Colors } from '../constants';
import { useTelemetry } from '../hooks/useTelemetry';
import EmergencyStop from '../components/EmergencyStop';

type Props = NativeStackScreenProps<RootStackParamList, 'Helm'>;

export default function HelmScreen({ route, navigation }: Props) {
  const { ip, cameraIP } = route.params;
  const { data, connected } = useTelemetry();
  useKeepAwake();

  return (
    <View style={styles.container}>
      {/* Status bar */}
      <View style={styles.statusRow}>
        <Text style={[styles.statusDot, { color: connected ? Colors.success : Colors.danger }]}>●</Text>
        <Text style={styles.statusText}>{connected ? 'CONNECTED' : 'OFFLINE'}</Text>
        <Text style={[styles.modeTag, data?.mode === 'AUTONOMOUS' && styles.modeAuto]}>
          {data?.mode ?? 'IDLE'}
        </Text>
      </View>

      {/* Key readouts */}
      <View style={styles.readoutRow}>
        <Readout label="HEADING" value={data ? `${data.heading}°` : '--'} />
        <Readout label="SPEED"   value={data ? `${data.speed_kts} kts` : '--'} />
        <Readout label="DEPTH"   value={data && data.sonar_ok ? `${data.depth_m} m` : '--'} />
        <Readout label="BATT"    value={data ? `${data.batt_v} V` : '--'} warn={data?.batt_low} />
      </View>

      {/* Bilge alarm */}
      {(data?.bilge_fwd || data?.bilge_aft) && (
        <View style={styles.alarm}>
          <Text style={styles.alarmText}>⚠ BILGE WATER DETECTED — PUMP {data?.pump ? 'ON' : 'OFF'}</Text>
        </View>
      )}

      {/* TODO: throttle slider + rudder joystick (Phase 1) */}
      <View style={styles.controlPlaceholder}>
        <Text style={styles.placeholder}>Throttle + Rudder controls (Phase 1)</Text>
      </View>

      {/* Bottom nav */}
      <View style={styles.navRow}>
        {(['Map', 'Telemetry', 'Weapons', 'Systems', 'FPV'] as const).map((screen) => (
          <TouchableOpacity
            key={screen}
            style={styles.navBtn}
            onPress={() => navigation.navigate(screen as any, { ip, cameraIP } as any)}
          >
            <Text style={styles.navBtnText}>{screen.toUpperCase()}</Text>
          </TouchableOpacity>
        ))}
      </View>

      <EmergencyStop ip={ip} />
    </View>
  );
}

function Readout({ label, value, warn }: { label: string; value: string; warn?: boolean }) {
  return (
    <View style={styles.readout}>
      <Text style={styles.readoutLabel}>{label}</Text>
      <Text style={[styles.readoutValue, warn && { color: Colors.warning }]}>{value}</Text>
    </View>
  );
}

const styles = StyleSheet.create({
  container:          { flex: 1, backgroundColor: Colors.background, padding: 16 },
  statusRow:          { flexDirection: 'row', alignItems: 'center', marginBottom: 12 },
  statusDot:          { fontSize: 10, marginRight: 6 },
  statusText:         { color: Colors.textSecondary, fontSize: 12, flex: 1 },
  modeTag:            { color: Colors.textPrimary, backgroundColor: Colors.surface, paddingHorizontal: 10, paddingVertical: 4, borderRadius: 4, fontSize: 12, fontWeight: 'bold' },
  modeAuto:           { backgroundColor: Colors.accent, color: '#000' },
  readoutRow:         { flexDirection: 'row', justifyContent: 'space-between', marginBottom: 16 },
  readout:            { alignItems: 'center', flex: 1 },
  readoutLabel:       { color: Colors.textSecondary, fontSize: 10, letterSpacing: 1 },
  readoutValue:       { color: Colors.textPrimary, fontSize: 20, fontWeight: 'bold', marginTop: 2 },
  alarm:              { backgroundColor: Colors.danger, padding: 10, borderRadius: 6, marginBottom: 12 },
  alarmText:          { color: '#fff', fontWeight: 'bold', textAlign: 'center' },
  controlPlaceholder: { flex: 1, justifyContent: 'center', alignItems: 'center' },
  placeholder:        { color: Colors.textSecondary, fontStyle: 'italic' },
  navRow:             { flexDirection: 'row', justifyContent: 'space-around', paddingBottom: 16 },
  navBtn:             { padding: 8 },
  navBtnText:         { color: Colors.accent, fontSize: 10, letterSpacing: 1 },
});
