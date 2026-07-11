import React, { useEffect, useRef, useState } from 'react';
import { View, Text, ScrollView, StyleSheet, TouchableOpacity, Alert } from 'react-native';
import { NativeStackScreenProps } from '@react-navigation/native-stack';
import { RootStackParamList } from '../../App';
import { Colors } from '../constants';
import { BATT_LOW_V, BATT_CRIT_V } from '../types';
import { useKeepAwake } from 'expo-keep-awake';
import { useTelemetry } from '../hooks/useTelemetry';
import {
  getRowCount, subscribeCount, exportShare, clear as clearLogger,
  start as startLogger, stop as stopLogger,
  isRunning, subscribeRunning, lastFrameAt, pendingGapCount,
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
    default:         return Colors.textSecondary;
  }
}

function magRowText(data: { mag_uT?: string; mag_baseline_uT?: string; mag_calibrated?: boolean }): string {
  const live = data.mag_uT != null ? parseFloat(data.mag_uT) : NaN;
  const base = data.mag_baseline_uT != null ? parseFloat(data.mag_baseline_uT) : NaN;
  const liveText = isNaN(live) ? '--' : `${live.toFixed(1)} µT`;
  if (data.mag_calibrated === false) return `${liveText} (uncal)`;
  if (!isNaN(live) && !isNaN(base) && base > 0) {
    const dev = ((live - base) / base) * 100;
    return `${liveText} (${dev >= 0 ? '+' : ''}${dev.toFixed(0)}%)`;
  }
  return liveText;
}

function magRowWarn(data: { mag_uT?: string; mag_baseline_uT?: string; mag_calibrated?: boolean }): boolean {
  if (data.mag_calibrated === false) return true;
  const live = data.mag_uT != null ? parseFloat(data.mag_uT) : NaN;
  const base = data.mag_baseline_uT != null ? parseFloat(data.mag_baseline_uT) : NaN;
  if (!isNaN(live) && !isNaN(base) && base > 0) {
    return Math.abs(live - base) / base > 0.25;
  }
  return false;
}

function fmtElapsed(ms: number): string {
  const s = Math.max(0, Math.floor(ms / 1000));
  return `${Math.floor(s / 60)}:${String(s % 60).padStart(2, '0')}`;
}

export default function TelemetryScreen({ navigation }: Props) {
  useKeepAwake();   // live telemetry surface; keep the screen (and its polling) awake in the field
  const { data, connected } = useTelemetry();
  const [logRows, setLogRows] = useState(getRowCount());
  const [logging, setLogging] = useState(isRunning());

  useEffect(() => subscribeCount(setLogRows),  []);
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
        { text: 'Clear',  style: 'destructive', onPress: clearLogger },
      ]
    );
  };

  return (
    <Screen>
      <View style={styles.screen}>

        {/* ── Top bar ───────────────────────────────────────────── */}
        <View style={styles.topBar}>
          <TouchableOpacity onPress={() => navigation.goBack()} style={styles.backBtn}>
            <Text style={styles.backBtnText}>‹ HELM</Text>
          </TouchableOpacity>
          <Text style={styles.pageTitle}>TELEMETRY</Text>
          <View style={styles.topBarRight}>
            <Text style={[styles.connDot, { color: connected ? Colors.success : Colors.danger }]}>●</Text>
            {data?.mode && (
              <View style={[styles.modeChip, { borderColor: modeColor(data.mode) }]}>
                <Text style={[styles.modeChipText, { color: modeColor(data.mode) }]}>
                  {data.mode}
                </Text>
              </View>
            )}
          </View>
        </View>

        {/* ── Contact / sync status ─────────────────────────────── */}
        <SyncStatus />

        {/* ── Battery card (prominent) ──────────────────────────── */}
        <View style={[styles.battCard, { borderLeftColor: voltageColor(battV) }]}>
          <Text style={styles.battLabel}>BATTERY</Text>
          <Text style={[styles.battValue, { color: voltageColor(battV) }]}>
            {battV != null ? `${battV.toFixed(2)}` : '--'}
            <Text style={styles.battUnit}>{battV != null ? ' V' : ''}</Text>
          </Text>
          <View style={styles.battMetaRow}>
            <Text style={styles.battThreshold}>
              warn &lt; {BATT_LOW_V} V   crit &lt; {BATT_CRIT_V} V
            </Text>
          </View>
        </View>

        {/* ── Failsafe ACK callout (conditional) ────────────────── */}
        {data?.failsafe_ack && (
          <View style={styles.ackCallout}>
            <Text style={styles.ackText}>FAILSAFE · ACK_REQUIRED — flip SwA UP</Text>
          </View>
        )}

        {/* ── Detail rows ───────────────────────────────────────── */}
        <ScrollView style={styles.scroll} showsVerticalScrollIndicator={false}>
          {data ? (
            <>
              <Section label="VESSEL" />
              <Row label="Firmware" value={data.v} />
              {data.uptime    != null && <Row label="Uptime"    value={`${data.uptime} s`} />}
              {data.heap      != null && <Row label="Free Heap" value={`${Math.round(data.heap / 1024)} KB`} />}
              {data.cruise_us != null && <Row label="Cruise"    value={`${data.cruise_us} µs`} />}
              {data.rc_age_ms != null && (
                <Row label="RC age" value={`${data.rc_age_ms} ms`} warn={data.rc_age_ms > 1000} />
              )}
              {data.rudder_us != null && <Row label="Rudder" value={`${data.rudder_us} µs`} />}
              {data.port_us   != null && <Row label="Port"   value={`${data.port_us} µs`} />}
              {data.stbd_us   != null && <Row label="Stbd"   value={`${data.stbd_us} µs`} />}
              {data.heading   != null && <Row label="Heading" value={`${data.heading}°`} />}
              {data.cog_trim  != null && parseFloat(data.cog_trim) !== 0 && (
                <Row label="COG trim" value={`${data.cog_trim}°`} />
              )}
              {(data.mag_uT != null || data.mag_calibrated != null) && (
                <Row label="Mag" value={magRowText(data)} warn={magRowWarn(data)} />
              )}

              <Section label="GPS" />
              <Row
                label="Fix"
                value={data.gps_fix ? '✓ LOCKED' : 'NO FIX'}
                warn={data.gps_fix === false}
              />
              {data.gps_fix && data.lat != null && data.lon != null && <Row label="Position" value={`${data.lat}, ${data.lon}`} />}
              {data.sats      != null && <Row label="Sats"   value={`${data.sats}`} />}
              {data.speed_kts != null && <Row label="Speed"  value={`${data.speed_kts} kts`} />}
              {data.course    != null && <Row label="Course" value={`${data.course}°`} />}

              <Section label="WAYPOINT" />
              {data.wp_set ? (
                <>
                  <Row label="Captured" value={data.captured ? 'true' : 'false'} />
                  {data.wp_dist_m  != null && <Row label="Distance" value={`${data.wp_dist_m} m`} />}
                  {data.wp_bearing != null && <Row label="Bearing"  value={`${data.wp_bearing}°`} />}
                </>
              ) : (
                <Text style={styles.sectionEmpty}>no waypoint set</Text>
              )}

              <Section label="PID" />
              {data.pid_kp != null && <Row label="Kp" value={data.pid_kp} />}
              {data.pid_kd != null && <Row label="Kd" value={data.pid_kd} />}

              <Section label="LIGHTS" />
              {data.nav_on    != null && <Row label="Nav"    value={data.nav_on    ? 'ON' : 'off'} />}
              {data.bridge_on != null && <Row label="Bridge" value={data.bridge_on ? 'ON' : 'off'} />}
              {data.deck_on   != null && <Row label="Deck"   value={data.deck_on   ? 'ON' : 'off'} />}
              {data.nav_on == null && data.bridge_on == null && data.deck_on == null && (
                <Text style={styles.empty}>(no light state in telemetry)</Text>
              )}

              {(data.bilge_fwd != null || data.bilge_mid != null || data.bilge_rear != null) && (
                <>
                  <Section label="BILGE" />
                  {data.bilge_fwd  != null && <Row label="Fwd"   value={data.bilge_fwd  ? '⚠ WET' : 'dry'} warn={data.bilge_fwd}  />}
                  {data.bilge_mid  != null && <Row label="Bilge" value={data.bilge_mid  ? '⚠ WET' : 'dry'} warn={data.bilge_mid}  />}
                  {data.bilge_rear != null && <Row label="Rear"  value={data.bilge_rear ? '⚠ WET' : 'dry'} warn={data.bilge_rear} />}
                  {data.pump_phase  != null && (
                    <Row label="Pump"
                         value={
                           data.pump_phase === 'off'
                             ? 'off'
                             : `${data.pump_phase.toUpperCase()} · cycle ${data.pump_cycle ?? '?'} · ${data.pump_manual ? 'MANUAL' : 'AUTO'}`
                         } />
                  )}
                  {data.pump_stuck && <Row label="Pump" value="⚠ stuck (auto cap hit)" warn />}
                </>
              )}
            </>
          ) : (
            <Text style={styles.empty}>Waiting for telemetry…</Text>
          )}
        </ScrollView>

        {/* ── Log bar ───────────────────────────────────────────── */}
        <View style={styles.logBar}>
          <TouchableOpacity
            style={[styles.logBtn, logging ? styles.logBtnStop : styles.logBtnStart]}
            onPress={logging ? stopLogger : startLogger}
            activeOpacity={0.7}
          >
            <Text style={logging ? styles.logBtnStopText : styles.logBtnStartText}>
              {logging ? '■ STOP' : '▶ START'}
            </Text>
          </TouchableOpacity>
          <Text style={styles.logCount}>{logRows} row{logRows === 1 ? '' : 's'}</Text>
          <TouchableOpacity
            style={[styles.logBtn, styles.logBtnClear]}
            onPress={onClear}
            disabled={logRows === 0}
            activeOpacity={0.7}
          >
            <Text style={[styles.logBtnClearText, logRows === 0 && { opacity: 0.4 }]}>CLEAR</Text>
          </TouchableOpacity>
          <TouchableOpacity
            style={[styles.logBtn, styles.logBtnExport]}
            onPress={onExport}
            disabled={logRows === 0}
            activeOpacity={0.7}
          >
            <Text style={[styles.logBtnExportText, logRows === 0 && { opacity: 0.4 }]}>EXPORT</Text>
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
  return (
    <View style={styles.sectionHeader}>
      <View style={styles.sectionHeaderRule} />
      <Text style={styles.sectionHeaderText}>{label}</Text>
      <View style={styles.sectionHeaderRule} />
    </View>
  );
}

// Contact / sync status bar (Telemetry screen only — the Helm screen stays
// uncluttered). Renders NOTHING while connected and caught up; appears only when
// it has something to say: how long since the last frame while out of contact,
// SYNCING… while the /history backfill drains on reconnect, then a brief
// SYNC COMPLETE. Pure read of logger state via a 1 Hz tick — changes nothing.
const CONTACT_STALE_MS = 4_000;         // no frame in this long ⇒ out of contact
const RING_WARN_MS     = 15 * 60_000;   // boat ring holds ~20 min; amber as it fills
const RING_CRIT_MS     = 20 * 60_000;   // past the ring — oldest un-synced data overwriting

function SyncStatus() {
  const [now, setNow] = useState(Date.now());
  useEffect(() => {
    const id = setInterval(() => setNow(Date.now()), 1000);
    return () => clearInterval(id);
  }, []);

  const last = lastFrameAt();
  const pending = pendingGapCount();
  const sinceContact = last > 0 ? now - last : Infinity;
  const inContact = sinceContact < CONTACT_STALE_MS;

  // Brief "SYNC COMPLETE" the moment the backfill queue drains from >0 to 0.
  const prevPending = useRef(0);
  const [syncedAt, setSyncedAt] = useState(0);
  useEffect(() => {
    if (prevPending.current > 0 && pending === 0) setSyncedAt(Date.now());
    prevPending.current = pending;
  }, [pending]);

  let text: string | null;
  let color: string = Colors.success;
  if (!inContact) {
    color = sinceContact > RING_CRIT_MS ? Colors.danger
          : sinceContact > RING_WARN_MS ? Colors.warning
          : Colors.textSecondary;
    text = last > 0 ? `NO CONTACT · ${fmtElapsed(sinceContact)}` : 'NO CONTACT';
  } else if (pending > 0) {
    color = Colors.warning;
    text = 'SYNCING…';
  } else if (now - syncedAt < 4_000) {
    text = 'SYNC COMPLETE ✓';
  } else {
    // Connected and caught up — nothing to report, so render nothing and stay
    // out of the way. The top-bar ● dot already signals "connected".
    text = null;
  }

  if (text == null) return null;

  return (
    <View style={[styles.syncBar, { borderColor: color }]}>
      <Text style={[styles.syncText, { color }]}>{text}</Text>
    </View>
  );
}

const styles = StyleSheet.create({
  screen: { flex: 1, paddingHorizontal: 16, paddingTop: 12 },

  // Top bar
  topBar:        { flexDirection: 'row', alignItems: 'center', marginBottom: 16 },
  backBtn:       { paddingVertical: 6, paddingRight: 12 },
  backBtnText:   { color: Colors.accent, fontSize: 12, fontFamily: 'monospace', letterSpacing: 2, fontWeight: '700' },
  pageTitle:     { flex: 1, textAlign: 'center', color: Colors.textPrimary, fontSize: 14, fontFamily: 'monospace', letterSpacing: 4, fontWeight: '800' },
  topBarRight:   { flexDirection: 'row', alignItems: 'center', gap: 6 },
  connDot:       { fontSize: 10 },
  modeChip:      { borderWidth: 1.5, borderRadius: 4, paddingHorizontal: 8, paddingVertical: 3, minWidth: 80, alignItems: 'center' },
  modeChipText:  { fontSize: 11, fontFamily: 'monospace', fontWeight: '800', letterSpacing: 2 },

  // Battery card
  battCard:       { backgroundColor: Colors.surface, borderRadius: 4, paddingVertical: 14, paddingHorizontal: 16, marginBottom: 12, borderLeftWidth: 3 },
  battLabel:      { color: Colors.textSecondary, fontSize: 10, fontFamily: 'monospace', letterSpacing: 3, fontWeight: '700' },
  battValue:      { fontSize: 38, fontFamily: 'monospace', fontWeight: '800', marginTop: 2, letterSpacing: -1 },
  battUnit:       { fontSize: 18, fontWeight: '700', letterSpacing: 1 },
  battMetaRow:    { flexDirection: 'row', alignItems: 'center', marginTop: 4, gap: 12 },
  battMeta:       { color: Colors.textSecondary, fontSize: 12, fontFamily: 'monospace' },
  battThreshold:  { color: Colors.textSecondary, fontSize: 10, fontFamily: 'monospace', opacity: 0.6 },

  // Contact / sync status bar
  syncBar:       { borderWidth: 1, borderRadius: 4, paddingVertical: 6, paddingHorizontal: 12, marginBottom: 12, alignItems: 'center', backgroundColor: Colors.surface },
  syncText:      { fontSize: 12, fontFamily: 'monospace', fontWeight: '800', letterSpacing: 2 },

  // Failsafe ACK callout
  ackCallout:    { backgroundColor: Colors.danger, padding: 10, borderRadius: 4, marginBottom: 12 },
  ackText:       { color: '#fff', fontSize: 12, fontFamily: 'monospace', fontWeight: '800', letterSpacing: 1, textAlign: 'center' },

  // Sections
  scroll: { flex: 1 },
  sectionHeader:     { flexDirection: 'row', alignItems: 'center', marginTop: 18, marginBottom: 8 },
  sectionHeaderRule: { flex: 1, height: 1, backgroundColor: Colors.surface },
  sectionHeaderText: { color: Colors.accent, fontSize: 10, letterSpacing: 4, fontFamily: 'monospace', fontWeight: '800', marginHorizontal: 12 },

  row:           { flexDirection: 'row', justifyContent: 'space-between', paddingVertical: 5 },
  label:         { color: Colors.textSecondary, fontSize: 13, fontFamily: 'monospace' },
  value:         { color: Colors.textPrimary, fontSize: 13, fontWeight: '700', fontFamily: 'monospace' },
  empty:         { color: Colors.textSecondary, textAlign: 'center', marginTop: 24, fontStyle: 'italic', fontFamily: 'monospace', fontSize: 12 },
  sectionEmpty:  { color: Colors.textSecondary, fontStyle: 'italic', fontFamily: 'monospace', fontSize: 12, paddingVertical: 6 },

  // Log bar
  logBar:           { flexDirection: 'row', alignItems: 'center', gap: 8, paddingTop: 10, marginTop: 4, borderTopWidth: 1, borderTopColor: Colors.surface },
  logBtn:           { paddingHorizontal: 12, paddingVertical: 9, borderRadius: 4 },
  logBtnStart:      { backgroundColor: Colors.surface, borderWidth: 1, borderColor: Colors.success },
  logBtnStop:       { backgroundColor: Colors.surface, borderWidth: 1, borderColor: Colors.warning },
  logBtnClear:      { backgroundColor: Colors.surface, borderWidth: 1, borderColor: Colors.danger },
  logBtnExport:     { backgroundColor: Colors.accent },
  logBtnStartText:  { color: Colors.success, fontSize: 11, fontFamily: 'monospace', fontWeight: '800', letterSpacing: 1 },
  logBtnStopText:   { color: Colors.warning, fontSize: 11, fontFamily: 'monospace', fontWeight: '800', letterSpacing: 1 },
  logBtnClearText:  { color: Colors.danger,  fontSize: 11, fontFamily: 'monospace', fontWeight: '800', letterSpacing: 1 },
  logBtnExportText: { color: '#000',         fontSize: 11, fontFamily: 'monospace', fontWeight: '800', letterSpacing: 2 },
  logCount:         { flex: 1, color: Colors.textSecondary, fontSize: 11, fontFamily: 'monospace', letterSpacing: 1, textAlign: 'center' },
});
