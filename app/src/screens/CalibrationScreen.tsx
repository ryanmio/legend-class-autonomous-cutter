// CalibrationScreen — drives the firmware's onboard mag calibration.
// All math happens on the boat; this screen is a remote trigger +
// status display. The operator rotates the whole boat through 360°
// while the firmware bins the horizontal field vector into 12 sectors;
// the cal finishes when every sector has been visited, so the progress
// shown here is actual rotation coverage, not elapsed time.

import React, { useCallback, useState } from 'react';
import { View, Text, StyleSheet, TouchableOpacity, Alert } from 'react-native';
import { NativeStackScreenProps } from '@react-navigation/native-stack';
import { RootStackParamList } from '../../App';
import { Colors } from '../constants';
import Screen from '../components/Screen';
import { useTelemetry } from '../hooks/useTelemetry';
import { postCalibrateMagStart, postCalibrateMagAbort } from '../services/esp32Service';

type Props = NativeStackScreenProps<RootStackParamList, 'Calibration'>;

const SECTOR_COUNT = 12;
// Mirrors firmware EXPECTED_HORIZ_FIELD_UT — local horizontal field, the
// yardstick the boat grades its own cal against.
const EXPECTED_RADIUS_UT = 20.5;

export default function CalibrationScreen({ route, navigation }: Props) {
  const { ip } = route.params;
  const { data, connected } = useTelemetry();
  const [busy, setBusy] = useState(false);

  const state    = data?.mag_cal_state ?? 'idle';
  const mask     = data?.mag_cal_mask ?? 0;
  const fail     = data?.mag_cal_fail;
  const quality  = data?.mag_cal_quality ?? 'unknown';
  const radius   = data?.mag_cal_radius_uT;
  const circ     = data?.mag_cal_circ_pct;
  const offX     = data?.mag_off_x;
  const offY     = data?.mag_off_y;
  const offZ     = data?.mag_off_z;
  const baseline = data?.mag_baseline_uT;
  const liveMag  = data?.mag_uT;
  const heading  = data?.heading;
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

        <View style={styles.statusBlock}>
          <Text style={styles.statusLabel}>STATUS</Text>
          <Text style={[styles.statusValue, { color: stateColor(state, calibrated, quality) }]}>
            {stateHeadline(state, calibrated, fromNVS)}
          </Text>
          {state === 'failed' && fail && (
            <Text style={styles.failReason}>{fail}</Text>
          )}
        </View>

        {state === 'collecting' ? (
          <CollectingBody mask={mask} liveMag={liveMag} onAbort={onAbort} busy={busy} />
        ) : (
          <>
            {quality !== 'unknown' && (
              <ResultsCard
                quality={quality}
                radius={radius}
                circ={circ}
                justFinished={state === 'done'}
              />
            )}
            <IdleBody
              state={state}
              calibrated={calibrated}
              offX={offX} offY={offY} offZ={offZ}
              baseline={baseline} liveMag={liveMag} heading={heading}
              onStart={onStart}
              busy={busy}
              disabled={!connected}
            />
          </>
        )}

        {state !== 'collecting' && (
          <View style={styles.instructionsBox}>
            <Text style={styles.instructionsTitle}>HOW TO CALIBRATE</Text>
            <Text style={styles.instructionsBody}>
              1. Calibrate in the water if possible — motors and hull where they'll be{'\n'}
              2. Keep clear of rebar, pumps, fences, and large metal{'\n'}
              3. Tap START, then turn the boat slowly through a full circle{'\n'}
              4. The ring fills as directions are covered — it finishes itself at 12/12{'\n'}
              5. Check the verdict: GOOD means the field circle matches this area's{'\n'}
              {'   '}expected ~{EXPECTED_RADIUS_UT} µT. POOR means re-run somewhere cleaner.
            </Text>
          </View>
        )}
      </View>
    </Screen>
  );
}

// ── State-driven sub-components ───────────────────────────────────────────────

// 12 dots on a ring, lit as the firmware's sector bitmap fills in. The
// dots are field-vector sectors, not compass directions — what matters
// is that all of them light up, in any order.
function CoverageRing({ mask }: { mask: number }) {
  const SIZE = 180;
  const R = 74;
  const covered = countBits(mask);
  const dots = [];
  for (let i = 0; i < SECTOR_COUNT; i++) {
    const lit = (mask & (1 << i)) !== 0;
    const a = (i / SECTOR_COUNT) * 2 * Math.PI - Math.PI / 2;
    dots.push(
      <View
        key={i}
        style={[
          styles.coverageDot,
          {
            left: SIZE / 2 + R * Math.cos(a) - 9,
            top:  SIZE / 2 + R * Math.sin(a) - 9,
            backgroundColor: lit ? Colors.accent : Colors.surfaceLight,
          },
        ]}
      />
    );
  }
  return (
    <View style={{ width: SIZE, height: SIZE, marginTop: 14 }}>
      {dots}
      <View style={styles.coverageCenter}>
        <Text style={styles.coverageCount}>{covered}/{SECTOR_COUNT}</Text>
        <Text style={styles.coverageCaption}>directions</Text>
      </View>
    </View>
  );
}

function CollectingBody({
  mask, liveMag, onAbort, busy,
}: {
  mask: number;
  liveMag?: string;
  onAbort: () => void;
  busy: boolean;
}) {
  return (
    <>
      <View style={styles.collectingBlock}>
        <Text style={styles.collectingTitle}>ROTATE THE BOAT</Text>
        <Text style={styles.collectingSub}>
          Turn slowly through a full circle. Dots light up as each direction
          is covered — if the ring stalls, keep turning. Finishes itself at 12/12.
        </Text>
        <CoverageRing mask={mask} />
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

// Post-cal verdict: the boat grades its own spin against the local field.
function ResultsCard({
  quality, radius, circ, justFinished,
}: {
  quality: 'good' | 'fair' | 'poor';
  radius?: string;
  circ?: string;
  justFinished: boolean;
}) {
  const color =
    quality === 'good' ? Colors.success
    : quality === 'fair' ? Colors.warning
    : Colors.danger;

  return (
    <View style={[styles.resultsCard, { borderColor: color }]}>
      <View style={styles.resultsHeader}>
        <Text style={[styles.resultsVerdict, { color }]}>{quality.toUpperCase()}</Text>
        <Text style={styles.resultsHeaderLabel}>
          {justFinished ? 'NEW CALIBRATION' : 'LAST CALIBRATION'}
        </Text>
      </View>
      {radius != null && (
        <ValueRow
          label="Field radius"
          value={`${radius} µT (expect ~${EXPECTED_RADIUS_UT})`}
        />
      )}
      {circ != null && (
        <ValueRow label="Lopsidedness" value={`${circ}% (soft iron)`} />
      )}
      <Text style={styles.resultsBlurb}>{qualityBlurb(quality)}</Text>
      {quality !== 'poor' && (
        <Text style={styles.verifyHint}>
          VERIFY: point the bow at a known direction and compare HEADING below
          with a compass app set to TRUE north (±10° is healthy).
        </Text>
      )}
    </View>
  );
}

function IdleBody({
  state, calibrated, offX, offY, offZ, baseline, liveMag, heading,
  onStart, busy, disabled,
}: {
  state: string;
  calibrated: boolean;
  offX?: string; offY?: string; offZ?: string;
  baseline?: string; liveMag?: string; heading?: string;
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
        <ValueRow label="Heading (true)" value={heading != null ? `${heading}°` : '--'} />
        <ValueRow label="Live |B|" value={liveMag != null ? `${liveMag} µT` : '--'} />
        <ValueRow
          label="Expected |B|"
          value={baseline != null && parseFloat(baseline) > 0 ? `${baseline} µT` : 'not calibrated'}
        />
        <ValueRow
          label="Offsets"
          value={offX != null ? `${offX} / ${offY} / ${offZ}` : '--'}
        />
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

function ValueRow({ label, value }: { label: string; value: string }) {
  return (
    <View style={styles.row}>
      <Text style={styles.rowLabel}>{label}</Text>
      <Text style={styles.rowValue}>{value}</Text>
    </View>
  );
}

// ── Display helpers ──────────────────────────────────────────────────────────

function countBits(mask: number): number {
  let n = 0;
  for (let i = 0; i < SECTOR_COUNT; i++) if (mask & (1 << i)) n++;
  return n;
}

function qualityBlurb(quality: 'good' | 'fair' | 'poor'): string {
  switch (quality) {
    case 'good':
      return 'Field circle matches the local earth field — heading should be trustworthy.';
    case 'fair':
      return 'Usable, but the field circle is off from what this area should read. '
           + 'Worth re-running farther from metal if heading still looks wrong.';
    case 'poor':
      return 'Offsets were saved, but the field circle is the wrong size or badly '
           + 'lopsided — something magnetic is distorting the sensor. Re-run the '
           + 'cal in open water away from structures before trusting AUTO.';
  }
}

function stateColor(state: string, calibrated: boolean, quality: string): string {
  switch (state) {
    case 'collecting': return Colors.accent;
    case 'done':       return quality === 'poor' ? Colors.warning : Colors.success;
    case 'failed':     return Colors.danger;
    case 'idle':
    default:
      return calibrated ? Colors.success : Colors.warning;
  }
}

function stateHeadline(state: string, calibrated: boolean, fromNVS: boolean): string {
  switch (state) {
    case 'collecting': return 'MEASURING ROTATION';
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

  resultsCard:        { backgroundColor: Colors.surface, borderRadius: 4, borderWidth: 1, padding: 14, marginBottom: 12 },
  resultsHeader:      { flexDirection: 'row', justifyContent: 'space-between', alignItems: 'center', marginBottom: 6 },
  resultsVerdict:     { fontSize: 24, fontFamily: 'monospace', fontWeight: '800', letterSpacing: 2 },
  resultsHeaderLabel: { color: Colors.textSecondary, fontSize: 10, fontFamily: 'monospace', letterSpacing: 2, fontWeight: '700' },
  resultsBlurb:       { color: Colors.textPrimary, fontSize: 12, fontFamily: 'monospace', lineHeight: 17, marginTop: 8 },
  verifyHint:         { color: Colors.textSecondary, fontSize: 11, fontFamily: 'monospace', lineHeight: 16, marginTop: 8 },

  valuesBlock:   { backgroundColor: Colors.surface, borderRadius: 4, padding: 14, marginBottom: 16 },
  row:           { flexDirection: 'row', justifyContent: 'space-between', paddingVertical: 4 },
  rowLabel:      { color: Colors.textSecondary, fontSize: 13, fontFamily: 'monospace' },
  rowValue:      { color: Colors.textPrimary, fontSize: 13, fontFamily: 'monospace', fontWeight: '700' },

  collectingBlock: { backgroundColor: Colors.surface, borderRadius: 4, padding: 18, marginBottom: 16, alignItems: 'center' },
  collectingTitle: { color: Colors.accent, fontSize: 16, fontFamily: 'monospace', fontWeight: '800', letterSpacing: 3 },
  collectingSub:   { color: Colors.textSecondary, fontSize: 12, fontFamily: 'monospace', textAlign: 'center', marginTop: 6, paddingHorizontal: 12 },
  coverageDot:     { position: 'absolute', width: 18, height: 18, borderRadius: 9 },
  coverageCenter:  { position: 'absolute', left: 0, right: 0, top: 0, bottom: 0, alignItems: 'center', justifyContent: 'center' },
  coverageCount:   { color: Colors.textPrimary, fontSize: 30, fontFamily: 'monospace', fontWeight: '800' },
  coverageCaption: { color: Colors.textSecondary, fontSize: 10, fontFamily: 'monospace', letterSpacing: 2 },
  liveMag:         { color: Colors.textSecondary, fontSize: 11, fontFamily: 'monospace', marginTop: 12 },

  primaryBtn:        { backgroundColor: Colors.accent, padding: 16, borderRadius: 4, alignItems: 'center' },
  primaryBtnDisabled:{ opacity: 0.4 },
  primaryBtnText:    { color: '#000', fontSize: 14, fontFamily: 'monospace', fontWeight: '800', letterSpacing: 2 },

  abortBtn:        { backgroundColor: Colors.surface, borderWidth: 1, borderColor: Colors.danger, padding: 14, borderRadius: 4, alignItems: 'center' },
  abortBtnText:    { color: Colors.danger, fontSize: 13, fontFamily: 'monospace', fontWeight: '800', letterSpacing: 2 },

  instructionsBox:   { marginTop: 20, padding: 12, backgroundColor: Colors.surface, borderRadius: 4 },
  instructionsTitle: { color: Colors.textSecondary, fontSize: 10, fontFamily: 'monospace', letterSpacing: 3, fontWeight: '700', marginBottom: 6 },
  instructionsBody:  { color: Colors.textPrimary, fontSize: 12, fontFamily: 'monospace', lineHeight: 18 },
});
