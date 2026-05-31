// CalibrationScreen — drives the firmware's onboard mag calibration.
// All math happens on the boat; this screen is a remote trigger +
// status display. The operator rotates the whole boat through 360° on
// a flat surface while the firmware collects samples and detects a
// plateau, then writes hard-iron offsets to NVS.

import React, { useCallback, useState } from 'react';
import { View, Text, StyleSheet, TouchableOpacity, Alert } from 'react-native';
import { NativeStackScreenProps } from '@react-navigation/native-stack';
import { RootStackParamList } from '../../App';
import { Colors } from '../constants';
import Screen from '../components/Screen';
import { useTelemetry } from '../hooks/useTelemetry';
import { postCalibrateMagStart, postCalibrateMagAbort } from '../services/esp32Service';

type Props = NativeStackScreenProps<RootStackParamList, 'Calibration'>;

export default function CalibrationScreen({ route, navigation }: Props) {
  const { ip } = route.params;
  const { data, connected } = useTelemetry();
  const [busy, setBusy] = useState(false);

  const state    = data?.mag_cal_state ?? 'idle';
  const progress = data?.mag_cal_progress ?? 0;
  const fail     = data?.mag_cal_fail;
  const offX     = data?.mag_off_x;
  const offY     = data?.mag_off_y;
  const offZ     = data?.mag_off_z;
  const baseline = data?.mag_baseline_uT;
  const liveMag  = data?.mag_uT;
  const calibrated = !!data?.mag_calibrated;
  const fromNVS    = !!data?.mag_from_nvs;

  const onStart = useCallback(async () => {
    setBusy(true);
    try {
      await postCalibrateMagStart(ip);
    } catch (e: any) {
      Alert.alert('Start failed', e?.message ?? String(e));
    } finally {
      setBusy(false);
    }
  }, [ip]);

  const onAbort = useCallback(async () => {
    setBusy(true);
    try {
      await postCalibrateMagAbort(ip);
    } catch (e: any) {
      Alert.alert('Abort failed', e?.message ?? String(e));
    } finally {
      setBusy(false);
    }
  }, [ip]);

  return (
    <Screen>
      <View style={styles.screen}>
        <View style={styles.topBar}>
          <TouchableOpacity onPress={() => navigation.goBack()} style={styles.backBtn}>
            <Text style={styles.backBtnText}>‹ SYSTEMS</Text>
          </TouchableOpacity>
          <Text style={styles.pageTitle}>COMPASS CAL</Text>
          <Text style={[styles.connDot, { color: connected ? Colors.success : Colors.danger }]}>●</Text>
        </View>

        {!connected && (
          <View style={styles.warnBox}>
            <Text style={styles.warnText}>OFFLINE — connect to the boat to calibrate.</Text>
          </View>
        )}

        {/* Current cal status header */}
        <View style={styles.statusBlock}>
          <Text style={styles.statusLabel}>STATUS</Text>
          <Text style={[styles.statusValue, { color: stateColor(state, calibrated) }]}>
            {stateHeadline(state, calibrated, fromNVS)}
          </Text>
          {state === 'failed' && fail && (
            <Text style={styles.failReason}>{fail}</Text>
          )}
        </View>

        {/* State-specific body */}
        {state === 'collecting' ? (
          <CollectingBody progress={progress} liveMag={liveMag} onAbort={onAbort} busy={busy} />
        ) : (
          <IdleBody
            state={state}
            calibrated={calibrated}
            offX={offX} offY={offY} offZ={offZ}
            baseline={baseline} liveMag={liveMag}
            onStart={onStart}
            busy={busy}
            disabled={!connected}
          />
        )}

        {/* Instructions footer (only when not actively collecting) */}
        {state !== 'collecting' && (
          <View style={styles.instructionsBox}>
            <Text style={styles.instructionsTitle}>HOW TO CALIBRATE</Text>
            <Text style={styles.instructionsBody}>
              1. Place the boat on a flat surface, away from large metal{'\n'}
              2. Tap START{'\n'}
              3. Rotate the boat slowly through 2–3 full 360° turns over ~30 s{'\n'}
              4. Wait — firmware auto-finishes when ranges plateau{'\n'}
              5. New offsets save to the boat's flash, survive reboot
            </Text>
          </View>
        )}
      </View>
    </Screen>
  );
}

// ── State-driven sub-components ───────────────────────────────────────────────

function IdleBody({
  state, calibrated, offX, offY, offZ, baseline, liveMag,
  onStart, busy, disabled,
}: {
  state: string;
  calibrated: boolean;
  offX?: string; offY?: string; offZ?: string;
  baseline?: string; liveMag?: string;
  onStart: () => void;
  busy: boolean;
  disabled: boolean;
}) {
  const buttonLabel =
    state === 'done'   ? '▶ RECALIBRATE'
    : state === 'failed' ? '▶ RETRY'
    : calibrated         ? '▶ RECALIBRATE'
                         : '▶ START CALIBRATION';

  return (
    <>
      <View style={styles.valuesBlock}>
        <ValueRow label="Offset X" value={offX != null ? `${offX} µT` : '--'} />
        <ValueRow label="Offset Y" value={offY != null ? `${offY} µT` : '--'} />
        <ValueRow label="Offset Z" value={offZ != null ? `${offZ} µT` : '--'} />
        <ValueRow label="Baseline" value={baseline != null && parseFloat(baseline) > 0 ? `${baseline} µT` : 'not calibrated'} />
        <ValueRow label="Live |B|" value={liveMag != null ? `${liveMag} µT` : '--'} />
      </View>

      <TouchableOpacity
        style={[styles.primaryBtn, (busy || disabled) && styles.primaryBtnDisabled]}
        onPress={onStart}
        disabled={busy || disabled}
        activeOpacity={0.7}
      >
        <Text style={styles.primaryBtnText}>{buttonLabel}</Text>
      </TouchableOpacity>
    </>
  );
}

function CollectingBody({
  progress, liveMag, onAbort, busy,
}: {
  progress: number;
  liveMag?: string;
  onAbort: () => void;
  busy: boolean;
}) {
  return (
    <>
      <View style={styles.collectingBlock}>
        <Text style={styles.collectingTitle}>ROTATE THE BOAT</Text>
        <Text style={styles.collectingSub}>
          Spin slowly through 2–3 full 360° turns on a flat surface.
        </Text>

        <View style={styles.progressTrack}>
          <View style={[styles.progressFill, { width: `${Math.min(100, progress)}%` }]} />
        </View>
        <Text style={styles.progressText}>{progress}%</Text>

        {liveMag != null && (
          <Text style={styles.liveMag}>live |B| = {liveMag} µT</Text>
        )}
      </View>

      <TouchableOpacity
        style={[styles.abortBtn, busy && styles.primaryBtnDisabled]}
        onPress={onAbort}
        disabled={busy}
        activeOpacity={0.7}
      >
        <Text style={styles.abortBtnText}>■ ABORT</Text>
      </TouchableOpacity>
    </>
  );
}

function ValueRow({ label, value }: { label: string; value: string }) {
  return (
    <View style={styles.row}>
      <Text style={styles.rowLabel}>{label}</Text>
      <Text style={styles.rowValue}>{value}</Text>
    </View>
  );
}

// ── Display helpers ──────────────────────────────────────────────────────────

function stateColor(state: string, calibrated: boolean): string {
  switch (state) {
    case 'collecting': return Colors.accent;
    case 'done':       return Colors.success;
    case 'failed':     return Colors.danger;
    case 'idle':
    default:
      return calibrated ? Colors.success : Colors.warning;
  }
}

function stateHeadline(state: string, calibrated: boolean, fromNVS: boolean): string {
  switch (state) {
    case 'collecting': return 'COLLECTING SAMPLES';
    case 'done':       return 'CALIBRATION COMPLETE';
    case 'failed':     return 'CALIBRATION FAILED';
    case 'idle':
    default:
      if (!calibrated)        return 'NOT CALIBRATED';
      if (fromNVS)            return 'CALIBRATED (FROM NVS)';
      return 'USING DEFAULTS';
  }
}

const styles = StyleSheet.create({
  screen: { flex: 1, paddingHorizontal: 16, paddingTop: 12 },

  topBar:        { flexDirection: 'row', alignItems: 'center', marginBottom: 16 },
  backBtn:       { paddingVertical: 6, paddingRight: 12 },
  backBtnText:   { color: Colors.accent, fontSize: 12, fontFamily: 'monospace', letterSpacing: 2, fontWeight: '700' },
  pageTitle:     { flex: 1, textAlign: 'center', color: Colors.textPrimary, fontSize: 14, fontFamily: 'monospace', letterSpacing: 4, fontWeight: '800' },
  connDot:       { fontSize: 10, minWidth: 40, textAlign: 'right' },

  warnBox:       { backgroundColor: Colors.danger, padding: 10, borderRadius: 4, marginBottom: 12 },
  warnText:      { color: '#fff', fontSize: 12, fontFamily: 'monospace', fontWeight: '800', letterSpacing: 1, textAlign: 'center' },

  statusBlock:   { backgroundColor: Colors.surface, borderRadius: 4, padding: 14, marginBottom: 12 },
  statusLabel:   { color: Colors.textSecondary, fontSize: 10, fontFamily: 'monospace', letterSpacing: 3, fontWeight: '700' },
  statusValue:   { fontSize: 22, fontFamily: 'monospace', fontWeight: '800', letterSpacing: 1, marginTop: 4 },
  failReason:    { color: Colors.danger, fontSize: 11, fontFamily: 'monospace', marginTop: 6 },

  valuesBlock:   { backgroundColor: Colors.surface, borderRadius: 4, padding: 14, marginBottom: 16 },
  row:           { flexDirection: 'row', justifyContent: 'space-between', paddingVertical: 4 },
  rowLabel:      { color: Colors.textSecondary, fontSize: 13, fontFamily: 'monospace' },
  rowValue:      { color: Colors.textPrimary, fontSize: 13, fontFamily: 'monospace', fontWeight: '700' },

  collectingBlock: { backgroundColor: Colors.surface, borderRadius: 4, padding: 18, marginBottom: 16, alignItems: 'center' },
  collectingTitle: { color: Colors.accent, fontSize: 16, fontFamily: 'monospace', fontWeight: '800', letterSpacing: 3 },
  collectingSub:   { color: Colors.textSecondary, fontSize: 12, fontFamily: 'monospace', textAlign: 'center', marginTop: 6, paddingHorizontal: 12 },
  progressTrack:   { width: '100%', height: 14, backgroundColor: Colors.surfaceLight, borderRadius: 7, marginTop: 18, overflow: 'hidden' },
  progressFill:    { height: '100%', backgroundColor: Colors.accent },
  progressText:    { color: Colors.textPrimary, fontSize: 28, fontFamily: 'monospace', fontWeight: '800', marginTop: 8 },
  liveMag:         { color: Colors.textSecondary, fontSize: 11, fontFamily: 'monospace', marginTop: 4 },

  primaryBtn:        { backgroundColor: Colors.accent, padding: 16, borderRadius: 4, alignItems: 'center' },
  primaryBtnDisabled:{ opacity: 0.4 },
  primaryBtnText:    { color: '#000', fontSize: 14, fontFamily: 'monospace', fontWeight: '800', letterSpacing: 2 },

  abortBtn:        { backgroundColor: Colors.surface, borderWidth: 1, borderColor: Colors.danger, padding: 14, borderRadius: 4, alignItems: 'center' },
  abortBtnText:    { color: Colors.danger, fontSize: 13, fontFamily: 'monospace', fontWeight: '800', letterSpacing: 2 },

  instructionsBox:   { marginTop: 20, padding: 12, backgroundColor: Colors.surface, borderRadius: 4 },
  instructionsTitle: { color: Colors.textSecondary, fontSize: 10, fontFamily: 'monospace', letterSpacing: 3, fontWeight: '700', marginBottom: 6 },
  instructionsBody:  { color: Colors.textPrimary, fontSize: 12, fontFamily: 'monospace', lineHeight: 18 },
});
