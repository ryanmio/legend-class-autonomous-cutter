import React, { useEffect, useState } from 'react';
import { View, Text, ScrollView, StyleSheet, TouchableOpacity, Alert } from 'react-native';
import { NativeStackScreenProps } from '@react-navigation/native-stack';
import { RootStackParamList } from '../../App';
import { Colors } from '../constants';
import { BATT_LOW_V, BATT_CRIT_V } from '../types';
import { useTelemetry } from '../hooks/useTelemetry';
import {
  getRowCount, subscribeCount, exportShare, clear as clearLogger,
  start as startLogger, stop as stopLogger,
  isRunning, subscribeRunning,
} from '../services/telemetryLogger';
import Screen from '../components/Screen';

type Props = NativeStackScreenProps<RootStackParamList, 'Telemetry'>;

// ── Helpers ───────────────────────────────────────────────────────────────────
function voltageColor(v: number | undefined): string {
  if (v == null || isNaN(v)) return Colors.textPrimary;
  if (v < BATT_CRIT_V) return Colors.danger;
  if (v < BATT_LOW_V)  return Colors.warning;
  return Colors.success;
}

function modeColor(mode: string | undefined): string {
  switch (mode) {
    case 'AUTO':     return Colors.success;
    case 'MANUAL':   return Colors.accent;
    case 'FAILSAFE': return Colors.danger;
    default:         return Colors.textPrimary;
  }
}

export default function TelemetryScreen({ route }: Props) {
  const { data, connected } = useTelemetry();
  const [logRows, setLogRows]   = useState(getRowCount());
  const [logging, setLogging]   = useState(isRunning());

  useEffect(() => subscribeCount(setLogRows), []);
  useEffect(() => subscribeRunning(setLogging), []);

  const battV = data?.batt_v != null ? parseFloat(data.batt_v) : undefined;

  const onExport = async () => {
    try { await exportShare(); }
    catch (e) { Alert.alert('Export failed', String(e)); }
  };
  const onClear = () => {
    Alert.alert(
      'Clear telemetry log?',
      `${logRows} row${logRows === 1 ? '' : 's'} will be discarded.`,
      [
        { text: 'Cancel', style: 'cancel' },
        { text: 'Clear', style: 'destructive', onPress: clearLogger },
      ]
    );
  };

  return (
    <Screen>
      <View style={styles.inner}>
        <View style={styles.header}>
          <Text style={styles.title}>
            TELEMETRY{'  '}
            <Text style={{ color: connected ? Colors.success : Colors.danger }}>●</Text>
          </Text>
        </View>

        {/* Voltage gets the prominent slot — it's the field most likely
            to surprise during a pool run. */}
        {battV != null && (
          <View style={styles.battCard}>
            <Text style={styles.battLabel}>BATTERY</Text>
            <Text style={[styles.battValue, { color: voltageColor(battV) }]}>
              {battV.toFixed(2)} V
            </Text>
            {data?.batt_a != null && (
              <Text style={styles.battSub}>{data.batt_a} A</Text>
            )}
            <Text style={styles.battThreshold}>
              warn &lt; {BATT_LOW_V} V    crit &lt; {BATT_CRIT_V} V
            </Text>
          </View>
        )}

        {data?.mode && (
          <View style={[styles.modeCard, { borderColor: modeColor(data.mode) }]}>
            <Text style={[styles.modeText, { color: modeColor(data.mode) }]}>
              {data.mode}
            </Text>
            {data.failsafe_ack && (
              <Text style={styles.ackText}>ACK_REQUIRED — flip SwA UP</Text>
            )}
          </View>
        )}

        <ScrollView style={styles.scroll}>
          {data && (
            <>
              <Row label="Firmware" value={data.v} />
              {data.uptime    != null && <Row label="Uptime"    value={`${data.uptime} s`} />}
              {data.heap      != null && <Row label="Free Heap" value={`${Math.round(data.heap / 1024)} KB`} />}
              {data.cruise_us != null && <Row label="Cruise"    value={`${data.cruise_us} µs`} />}
              {data.rc_age_ms != null && (
                <Row
                  label="RC age"
                  value={`${data.rc_age_ms} ms`}
                  warn={data.rc_age_ms > 1000}
                />
              )}
              {data.rudder_us != null && <Row label="Rudder"    value={`${data.rudder_us} µs`} />}
              {data.esc_us    != null && <Row label="ESC"       value={`${data.esc_us} µs`} />}
              {data.heading   != null && <Row label="Heading"   value={`${data.heading}°`} />}

              <Section label="GPS" />
              <Row label="Fix" value={data.gps_fix ? '✓' : '✗'} warn={data.gps_fix === false} />
              {data.gps_simulated && <Row label="Simulated" value="⚠ /sim_gps active" warn />}
              {data.gps_fix && data.lat != null && <Row label="Position" value={`${data.lat}, ${data.lon}`} />}
              {data.sats      != null && <Row label="Sats"      value={`${data.sats}`} />}
              {data.speed_kts != null && <Row label="Speed"     value={`${data.speed_kts} kts`} />}
              {data.course    != null && <Row label="Course"    value={`${data.course}°`} />}

              <Section label="Waypoint" />
              <Row label="Set"      value={data.wp_set ? '✓' : '✗'} />
              <Row label="Captured" value={data.captured ? '✓' : '✗'} />
              {data.wp_dist_m  != null && <Row label="Distance" value={`${data.wp_dist_m} m`} />}
              {data.wp_bearing != null && <Row label="Bearing"  value={`${data.wp_bearing}°`} />}

              <Section label="PID" />
              {data.pid_kp != null && <Row label="Kp" value={data.pid_kp} />}
              {data.pid_kd != null && <Row label="Kd" value={data.pid_kd} />}

              <Section label="Lights" />
              {data.nav_on    != null && <Row label="Nav"    value={data.nav_on    ? 'ON' : 'off'} />}
              {data.bridge_on != null && <Row label="Bridge" value={data.bridge_on ? 'ON' : 'off'} />}
              {data.deck_on   != null && <Row label="Deck"   value={data.deck_on   ? 'ON' : 'off'} />}

              {(data.bilge_fwd != null || data.bilge_aft != null) && (
                <>
                  <Section label="Bilge" />
                  {data.bilge_fwd != null && <Row label="Fwd" value={data.bilge_fwd ? '⚠ WET' : 'dry'} warn={data.bilge_fwd} />}
                  {data.bilge_aft != null && <Row label="Aft" value={data.bilge_aft ? '⚠ WET' : 'dry'} warn={data.bilge_aft} />}
                  {data.pump      != null && <Row label="Pump" value={data.pump ? 'RUNNING' : 'off'} />}
                </>
              )}
            </>
          )}
          {!data && <Text style={styles.empty}>Waiting for telemetry…</Text>}
        </ScrollView>

        <View style={styles.logBar}>
          <TouchableOpacity
            style={[styles.logBtn, logging ? styles.logBtnStop : styles.logBtnStart]}
            onPress={logging ? stopLogger : startLogger}
          >
            <Text style={logging ? styles.logBtnStopText : styles.logBtnStartText}>
              {logging ? '■ STOP' : '▶ START'}
            </Text>
          </TouchableOpacity>
          <Text style={styles.logCount}>{logRows} row{logRows === 1 ? '' : 's'}</Text>
          <TouchableOpacity style={[styles.logBtn, styles.logBtnSecondary]} onPress={onClear} disabled={logRows === 0}>
            <Text style={[styles.logBtnText, logRows === 0 && { opacity: 0.4 }]}>CLEAR</Text>
          </TouchableOpacity>
          <TouchableOpacity style={[styles.logBtn, styles.logBtnPrimary]} onPress={onExport} disabled={logRows === 0}>
            <Text style={[styles.logBtnPrimaryText, logRows === 0 && { opacity: 0.4 }]}>EXPORT CSV</Text>
          </TouchableOpacity>
        </View>
      </View>
    </Screen>
  );
}

// ── Layout helpers ────────────────────────────────────────────────────────────
function Row({ label, value, warn }: { label: string; value: string | number; warn?: boolean }) {
  return (
    <View style={styles.row}>
      <Text style={styles.label}>{label}</Text>
      <Text style={[styles.value, warn && { color: Colors.warning }]}>{String(value)}</Text>
    </View>
  );
}
function Section({ label }: { label: string }) {
  return <Text style={styles.section}>{label}</Text>;
}

const styles = StyleSheet.create({
  inner:   { flex: 1, padding: 16 },
  header:  { flexDirection: 'row', justifyContent: 'space-between', alignItems: 'center', marginBottom: 8 },
  title:   { color: Colors.accent, fontSize: 18, fontWeight: 'bold' },

  battCard: {
    backgroundColor: Colors.surface,
    borderRadius: 8,
    padding: 12,
    marginBottom: 10,
  },
  battLabel: {
    color: Colors.textSecondary, fontSize: 11, fontFamily: 'monospace',
    letterSpacing: 2,
  },
  battValue: {
    fontSize: 32, fontFamily: 'monospace', fontWeight: 'bold',
    marginTop: 2,
  },
  battSub: {
    color: Colors.textSecondary, fontSize: 13, fontFamily: 'monospace',
  },
  battThreshold: {
    color: Colors.textSecondary, fontSize: 10, fontFamily: 'monospace',
    marginTop: 4,
  },

  modeCard: {
    backgroundColor: Colors.surface,
    borderRadius: 8,
    padding: 10,
    marginBottom: 10,
    borderWidth: 1.5,
    alignItems: 'center',
  },
  modeText: {
    fontSize: 18, fontFamily: 'monospace', fontWeight: 'bold', letterSpacing: 4,
  },
  ackText: {
    color: Colors.danger, fontSize: 11, fontFamily: 'monospace', marginTop: 4,
  },

  scroll: { flex: 1 },
  section: {
    color: Colors.textSecondary,
    fontSize: 11,
    fontFamily: 'monospace',
    letterSpacing: 2,
    marginTop: 14,
    marginBottom: 4,
    paddingTop: 6,
    borderTopWidth: 1,
    borderTopColor: Colors.surface,
  },
  row:    { flexDirection: 'row', justifyContent: 'space-between', paddingVertical: 5 },
  label:  { color: Colors.textSecondary, fontSize: 13 },
  value:  { color: Colors.textPrimary, fontSize: 13, fontWeight: '600', fontFamily: 'monospace' },
  empty:  { color: Colors.textSecondary, textAlign: 'center', marginTop: 40, fontStyle: 'italic' },

  logBar: {
    flexDirection: 'row', alignItems: 'center', gap: 8,
    paddingTop: 10, marginTop: 4,
    borderTopWidth: 1, borderTopColor: Colors.surface,
  },
  logCount:        { flex: 1, color: Colors.textSecondary, fontSize: 11, fontFamily: 'monospace', letterSpacing: 1, textAlign: 'center' },
  logBtn:          { paddingHorizontal: 12, paddingVertical: 9, borderRadius: 6 },
  logBtnStart:     { backgroundColor: Colors.surface, borderWidth: 1, borderColor: Colors.success },
  logBtnStop:      { backgroundColor: Colors.surface, borderWidth: 1, borderColor: Colors.warning },
  logBtnStartText: { color: Colors.success, fontSize: 11, fontFamily: 'monospace', fontWeight: 'bold', letterSpacing: 1 },
  logBtnStopText:  { color: Colors.warning, fontSize: 11, fontFamily: 'monospace', fontWeight: 'bold', letterSpacing: 1 },
  logBtnSecondary: { backgroundColor: Colors.surface, borderWidth: 1, borderColor: Colors.danger },
  logBtnPrimary:   { backgroundColor: Colors.accent },
  logBtnText:      { color: Colors.danger, fontSize: 11, fontFamily: 'monospace', fontWeight: 'bold', letterSpacing: 1 },
  logBtnPrimaryText:{ color: '#000', fontSize: 11, fontFamily: 'monospace', fontWeight: 'bold', letterSpacing: 1 },
});
