import React from 'react';
import { View, Text, StyleSheet, TouchableOpacity } from 'react-native';
import { NativeStackScreenProps } from '@react-navigation/native-stack';
import { useKeepAwake } from 'expo-keep-awake';
import { RootStackParamList } from '../../App';
import { Colors } from '../constants';
import { useTelemetry } from '../hooks/useTelemetry';
import { setLed } from '../services/esp32Service';
import Screen from '../components/Screen';

const LIGHTS = [
  { key: 'nav'    as const, label: 'NAV'    },
  { key: 'bridge' as const, label: 'BRIDGE' },
  { key: 'deck'   as const, label: 'DECK'   },
];

type Props = NativeStackScreenProps<RootStackParamList, 'Helm'>;

export default function HelmScreen({ route, navigation }: Props) {
  const { ip } = route.params;
  const { data, connected } = useTelemetry();
  useKeepAwake();

  return (
    <Screen>
      <View style={styles.inner}>
        <View style={styles.statusRow}>
          <Text style={[styles.statusDot, { color: connected ? Colors.success : Colors.danger }]}>●</Text>
          <Text style={styles.statusText}>{connected ? 'CONNECTED' : 'OFFLINE'}</Text>
          <Text style={[styles.modeTag, data?.mode === 'AUTO' && styles.modeAuto]}>
            {data?.mode ?? 'IDLE'}
          </Text>
        </View>

        <View style={styles.readoutRow}>
          <Readout label="HEADING" value={data?.heading   != null ? `${data.heading}°`      : '--'} />
          <Readout label="SPEED"   value={data?.speed_kts != null ? `${data.speed_kts} kts` : '--'} />
          <Readout label="DEPTH"   value={data?.sonar_ok && data?.depth_m != null ? `${data.depth_m} m` : '--'} />
          <Readout label="BATT"    value={data?.batt_v   != null ? `${data.batt_v} V`       : '--'} warn={data?.batt_low} />
        </View>

        {(data?.bilge_fwd || data?.bilge_aft) && (
          <View style={styles.alarm}>
            <Text style={styles.alarmText}>⚠ BILGE WATER DETECTED — PUMP {data?.pump ? 'ON' : 'OFF'}</Text>
          </View>
        )}

        <View style={styles.lightsSection}>
          <Text style={styles.sectionLabel}>LIGHTS</Text>
          <View style={styles.lightsRow}>
            {LIGHTS.map(({ key, label }) => {
              const on = (key === 'nav'    ? data?.nav_on
                       :  key === 'bridge' ? data?.bridge_on
                       :                     data?.deck_on) ?? false;
              return (
                <TouchableOpacity
                  key={key}
                  style={[styles.lightBtn, on && styles.lightBtnOn]}
                  onPress={() => setLed(ip, key, !on).catch(() => {})}
                >
                  <Text style={[styles.lightBtnText, on && styles.lightBtnTextOn]}>
                    {on ? `${label} ON` : label}
                  </Text>
                </TouchableOpacity>
              );
            })}
          </View>
        </View>

        <View style={styles.navRow}>
          {(['Map', 'Telemetry', 'Weapons', 'Survey', 'Settings'] as const).map((screen) => (
            <TouchableOpacity
              key={screen}
              style={styles.navBtn}
              onPress={() => navigation.navigate(screen, { ip })}
            >
              <Text style={styles.navBtnText}>{screen.toUpperCase()}</Text>
            </TouchableOpacity>
          ))}
        </View>
      </View>
    </Screen>
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
  inner:              { flex: 1, padding: 16 },
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
  lightsSection:      { flex: 1, justifyContent: 'flex-start' },
  sectionLabel:       { color: Colors.textSecondary, fontSize: 11, letterSpacing: 2, marginBottom: 8 },
  lightsRow:          { flexDirection: 'row', gap: 12 },
  lightBtn:           { flex: 1, backgroundColor: Colors.surface, padding: 18, borderRadius: 8, alignItems: 'center' },
  lightBtnOn:         { backgroundColor: Colors.accent },
  lightBtnText:       { color: Colors.textSecondary, fontWeight: 'bold', fontSize: 13, letterSpacing: 1 },
  lightBtnTextOn:     { color: '#000' },
  navRow:             { flexDirection: 'row', justifyContent: 'space-around', paddingVertical: 8 },
  navBtn:             { padding: 8 },
  navBtnText:         { color: Colors.accent, fontSize: 10, letterSpacing: 1 },
});
