import React, { useCallback, useEffect, useState } from 'react';
import { View, Text, StyleSheet, TouchableOpacity } from 'react-native';
import { NativeStackScreenProps } from '@react-navigation/native-stack';
import { useKeepAwake } from 'expo-keep-awake';
import { RootStackParamList } from '../../App';
import { Colors } from '../constants';
import { BATT_LOW_V, BATT_CRIT_V } from '../types';
import { useTelemetry } from '../hooks/useTelemetry';
import { setLed, setCruise as sendCruise } from '../services/esp32Service';
import { subscribeRunning } from '../services/telemetryLogger';
import { CruiseModal } from '../components/CruiseModal';
import Screen from '../components/Screen';

type LightKey = 'nav' | 'bridge' | 'deck';

const LIGHTS = [
  { key: 'nav'    as const, label: 'NAV'    },
  { key: 'bridge' as const, label: 'BRIDGE' },
  { key: 'deck'   as const, label: 'DECK'   },
];

function voltageColor(v: number | undefined): string | undefined {
  if (v == null || isNaN(v)) return undefined;
  if (v < BATT_CRIT_V) return Colors.danger;
  if (v < BATT_LOW_V)  return Colors.warning;
  return Colors.success;
}

type Props = NativeStackScreenProps<RootStackParamList, 'Helm'>;

export default function HelmScreen({ route, navigation }: Props) {
  const { ip } = route.params;
  const { data, connected } = useTelemetry();
  const [logging, setLogging] = useState(false);
  const [cruiseModalOpen, setCruiseModalOpen] = useState(false);
  // Local optimistic light state. test_29 firmware doesn't echo nav_on /
  // bridge_on / deck_on yet, so taps would visually do nothing without
  // this. When production firmware sends the fields, replace this with
  // a server-state-with-pending-override pattern.
  const [localLights, setLocalLights] = useState<Record<LightKey, boolean>>({
    nav: false, bridge: false, deck: false,
  });
  useKeepAwake();

  useEffect(() => subscribeRunning(setLogging), []);

  const battV    = data?.batt_v != null ? parseFloat(data.batt_v) : undefined;
  const cruiseUs = data?.cruise_us;
  const wpDist   = data?.wp_set && data?.wp_dist_m != null ? `${data.wp_dist_m} m` : '--';

  const handlePickCruise = useCallback((us: number) => {
    sendCruise(ip, { us }).catch(() => {});
    setCruiseModalOpen(false);
  }, [ip]);

  const toggleLight = useCallback((key: LightKey) => {
    setLocalLights((prev) => {
      const next = !prev[key];
      setLed(ip, key, next).catch(() => {});
      return { ...prev, [key]: next };
    });
  }, [ip]);

  return (
    <Screen>
      <View style={styles.inner}>
        <View style={styles.statusRow}>
          <Text style={[styles.statusDot, { color: connected ? Colors.success : Colors.danger }]}>●</Text>
          <Text style={styles.statusText}>{connected ? 'CONNECTED' : 'OFFLINE'}</Text>
          {logging && (
            <View style={styles.logPill}>
              <Text style={styles.logPillDot}>●</Text>
              <Text style={styles.logPillText}>LOG</Text>
            </View>
          )}
          <Text style={[styles.modeTag, data?.mode === 'AUTO' && styles.modeAuto]}>
            {data?.mode ?? 'IDLE'}
          </Text>
        </View>

        <View style={styles.readoutRow}>
          <Readout label="HEADING" value={data?.heading   != null ? `${data.heading}°`      : '--'} />
          <Readout label="SPEED"   value={data?.speed_kts != null ? `${data.speed_kts} kts` : '--'} />
          <Readout label="DEPTH"   value={data?.sonar_ok && data?.depth_m != null ? `${data.depth_m} m` : '--'} />
          <Readout label="BATT"    value={battV != null ? `${battV.toFixed(2)} V` : '--'} color={voltageColor(battV)} />
        </View>

        {(data?.bilge_fwd || data?.bilge_aft) && (
          <View style={styles.alarm}>
            <Text style={styles.alarmText}>⚠ BILGE WATER DETECTED — PUMP {data?.pump ? 'ON' : 'OFF'}</Text>
          </View>
        )}

        <Text style={styles.sectionLabel}>AUTOPILOT</Text>
        <View style={styles.autopilotRow}>
          <TouchableOpacity style={styles.apCard} onPress={() => setCruiseModalOpen(true)}>
            <Text style={styles.apCardLabel}>CRUISE</Text>
            <Text style={styles.apCardValue}>{cruiseUs != null ? `${cruiseUs} µs` : '--'}</Text>
          </TouchableOpacity>
          <View style={styles.apCard}>
            <Text style={styles.apCardLabel}>WP DIST</Text>
            <Text style={styles.apCardValue}>{wpDist}</Text>
          </View>
        </View>

        <View style={styles.lightsSection}>
          <Text style={styles.sectionLabel}>LIGHTS</Text>
          <View style={styles.lightsRow}>
            {LIGHTS.map(({ key, label }) => {
              const on = localLights[key];
              return (
                <TouchableOpacity
                  key={key}
                  style={[styles.lightBtn, on && styles.lightBtnOn]}
                  onPress={() => toggleLight(key)}
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

      <CruiseModal
        visible={cruiseModalOpen}
        currentUs={cruiseUs}
        onPick={handlePickCruise}
        onCancel={() => setCruiseModalOpen(false)}
      />
    </Screen>
  );
}

function Readout({ label, value, color }: { label: string; value: string; color?: string }) {
  return (
    <View style={styles.readout}>
      <Text style={styles.readoutLabel}>{label}</Text>
      <Text style={[styles.readoutValue, color != null && { color }]}>{value}</Text>
    </View>
  );
}

const styles = StyleSheet.create({
  inner:              { flex: 1, padding: 16 },
  statusRow:          { flexDirection: 'row', alignItems: 'center', marginBottom: 12 },
  statusDot:          { fontSize: 10, marginRight: 6 },
  statusText:         { color: Colors.textSecondary, fontSize: 12, flex: 1 },
  logPill:            { flexDirection: 'row', alignItems: 'center', backgroundColor: Colors.surface, paddingHorizontal: 8, paddingVertical: 3, borderRadius: 4, marginRight: 8 },
  logPillDot:         { color: Colors.success, fontSize: 9, marginRight: 4 },
  logPillText:        { color: Colors.success, fontSize: 11, fontWeight: 'bold', letterSpacing: 1, fontFamily: 'monospace' },
  modeTag:            { color: Colors.textPrimary, backgroundColor: Colors.surface, paddingHorizontal: 10, paddingVertical: 4, borderRadius: 4, fontSize: 12, fontWeight: 'bold' },
  modeAuto:           { backgroundColor: Colors.accent, color: '#000' },
  readoutRow:         { flexDirection: 'row', justifyContent: 'space-between', marginBottom: 16 },
  readout:            { alignItems: 'center', flex: 1 },
  readoutLabel:       { color: Colors.textSecondary, fontSize: 10, letterSpacing: 1 },
  readoutValue:       { color: Colors.textPrimary, fontSize: 20, fontWeight: 'bold', marginTop: 2 },
  alarm:              { backgroundColor: Colors.danger, padding: 10, borderRadius: 6, marginBottom: 12 },
  alarmText:          { color: '#fff', fontWeight: 'bold', textAlign: 'center' },
  autopilotRow:       { flexDirection: 'row', gap: 12, marginBottom: 16 },
  apCard:             { flex: 1, backgroundColor: Colors.surface, padding: 12, borderRadius: 8, alignItems: 'center' },
  apCardLabel:        { color: Colors.textSecondary, fontSize: 10, letterSpacing: 1 },
  apCardValue:        { color: Colors.accent, fontSize: 18, fontWeight: 'bold', fontFamily: 'monospace', marginTop: 2 },
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
