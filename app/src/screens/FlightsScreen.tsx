// FlightsScreen — lifetime totals at top, list of saved flights below.
// Tap a row to open the detail screen; long-press (or trailing ×) deletes.

import React, { useCallback, useEffect, useState } from 'react';
import {
  View, Text, StyleSheet, TouchableOpacity, FlatList, Alert, RefreshControl,
} from 'react-native';
import { NativeStackScreenProps } from '@react-navigation/native-stack';
import { RootStackParamList } from '../../App';
import { Colors } from '../constants';
import Screen from '../components/Screen';
import { FlightMeta, listFlights, deleteFlight } from '../services/telemetryLogger';

type Props = NativeStackScreenProps<RootStackParamList, 'Flights'>;

// ── Format helpers ────────────────────────────────────────────────────────

function fmtTimestamp(ms: number): string {
  const d = new Date(ms);
  const pad = (n: number) => String(n).padStart(2, '0');
  return `${d.getFullYear()}-${pad(d.getMonth() + 1)}-${pad(d.getDate())} ${pad(d.getHours())}:${pad(d.getMinutes())}`;
}

function fmtDuration(ms: number): string {
  const totalSec = Math.max(0, Math.round(ms / 1000));
  const h = Math.floor(totalSec / 3600);
  const m = Math.floor((totalSec % 3600) / 60);
  const s = totalSec % 60;
  if (h > 0) return `${h}h ${pad2(m)}m`;
  if (m > 0) return `${m}m ${pad2(s)}s`;
  return `${s}s`;
}
function pad2(n: number) { return String(n).padStart(2, '0'); }

function fmtDistance(m: number | undefined): string {
  if (m == null) return '—';
  if (m >= 1000) return `${(m / 1000).toFixed(2)} km`;
  return `${Math.round(m)} m`;
}

function fmtSpeed(kts: number | undefined): string {
  if (kts == null || kts <= 0) return '—';
  return `${kts.toFixed(1)} kts`;
}

// ── Lifetime stats reducer ────────────────────────────────────────────────

interface Lifetime {
  count:      number;
  totalMs:    number;
  totalM:     number;
  maxKts:     number;
  autoMs:     number;
  captures:   number;
}
function summarize(flights: FlightMeta[]): Lifetime {
  let totalMs = 0, totalM = 0, maxKts = 0, autoMs = 0, captures = 0;
  for (const f of flights) {
    totalMs += Math.max(0, f.endTs - f.startTs);
    totalM  += f.distanceM   ?? 0;
    autoMs  += f.autoMs      ?? 0;
    if ((f.maxSpeedKts ?? 0) > maxKts) maxKts = f.maxSpeedKts ?? 0;
    if (f.capturedWaypoint) captures++;
  }
  return { count: flights.length, totalMs, totalM, maxKts, autoMs, captures };
}

// ── Screen ────────────────────────────────────────────────────────────────

export default function FlightsScreen({ navigation }: Props) {
  const [flights, setFlights]   = useState<FlightMeta[]>([]);
  const [refreshing, setRefresh] = useState(false);
  const [loading,    setLoading] = useState(true);

  const load = useCallback(async () => {
    const list = await listFlights();
    list.sort((a, b) => b.startTs - a.startTs);  // newest first
    setFlights(list);
    setLoading(false);
  }, []);

  useEffect(() => { load(); }, [load]);

  // Reload when the screen regains focus — e.g., after deleting from
  // the detail screen and navigating back.
  useEffect(() => {
    const unsub = navigation.addListener('focus', () => { load(); });
    return unsub;
  }, [navigation, load]);

  const onRefresh = useCallback(async () => {
    setRefresh(true);
    await load();
    setRefresh(false);
  }, [load]);

  const onDelete = useCallback((meta: FlightMeta) => {
    Alert.alert(
      'Delete flight?',
      `${fmtTimestamp(meta.startTs)} · ${meta.rowCount} rows. This cannot be undone.`,
      [
        { text: 'Cancel', style: 'cancel' },
        {
          text: 'Delete',
          style: 'destructive',
          onPress: async () => {
            await deleteFlight(meta.id);
            await load();
          },
        },
      ],
    );
  }, [load]);

  const life = summarize(flights);

  return (
    <Screen>
      <View style={styles.screen}>
        <View style={styles.topBar}>
          <TouchableOpacity onPress={() => navigation.goBack()} style={styles.backBtn}>
            <Text style={styles.backBtnText}>‹ BACK</Text>
          </TouchableOpacity>
          <Text style={styles.pageTitle}>FLIGHT LOGS</Text>
          <View style={styles.topBarRight}>
            <Text style={styles.countText}>{flights.length}</Text>
          </View>
        </View>

        <FlatList
          data={flights}
          keyExtractor={(m) => m.id}
          ListHeaderComponent={
            flights.length === 0 ? null : <LifetimeCard life={life} />
          }
          refreshControl={
            <RefreshControl refreshing={refreshing} onRefresh={onRefresh} tintColor={Colors.accent} />
          }
          ListEmptyComponent={
            loading ? null : (
              <Text style={styles.empty}>
                No flights recorded yet.{'\n\n'}
                Logging starts automatically when the boat is throttled up
                and ends when telemetry is lost for 5 min, the boat reboots,
                or you tap STOP on Telemetry.
              </Text>
            )
          }
          contentContainerStyle={flights.length === 0 ? styles.emptyContainer : undefined}
          renderItem={({ item }) => (
            <TouchableOpacity
              style={styles.row}
              activeOpacity={0.7}
              onPress={() => navigation.navigate('FlightDetail', { id: item.id })}
              onLongPress={() => onDelete(item)}
            >
              <View style={{ flex: 1 }}>
                <Text style={styles.rowTimestamp}>{fmtTimestamp(item.startTs)}</Text>
                <Text style={styles.rowMeta}>
                  {fmtDuration(item.endTs - item.startTs)}
                  {item.distanceM != null && ` · ${fmtDistance(item.distanceM)}`}
                  {item.maxSpeedKts != null && item.maxSpeedKts > 0 && ` · max ${fmtSpeed(item.maxSpeedKts)}`}
                  {item.capturedWaypoint ? '  ✓ WP' : ''}
                </Text>
              </View>
              <TouchableOpacity onPress={() => onDelete(item)} style={styles.deleteBtn} hitSlop={8}>
                <Text style={styles.deleteBtnText}>×</Text>
              </TouchableOpacity>
            </TouchableOpacity>
          )}
        />
      </View>
    </Screen>
  );
}

// ── Lifetime card ─────────────────────────────────────────────────────────

function LifetimeCard({ life }: { life: Lifetime }) {
  return (
    <View style={styles.lifetimeCard}>
      <Text style={styles.lifetimeTitle}>LIFETIME</Text>
      <View style={styles.lifetimeRow}>
        <Stat label="FLIGHTS"  value={String(life.count)} />
        <Stat label="TIME"     value={fmtDuration(life.totalMs)} />
        <Stat label="DISTANCE" value={fmtDistance(life.totalM)} />
      </View>
      <View style={styles.lifetimeRow}>
        <Stat label="MAX SPEED" value={fmtSpeed(life.maxKts)} />
        <Stat label="AUTO"      value={fmtDuration(life.autoMs)} />
        <Stat label="CAPTURES"  value={String(life.captures)} />
      </View>
    </View>
  );
}

function Stat({ label, value }: { label: string; value: string }) {
  return (
    <View style={styles.stat}>
      <Text style={styles.statLabel}>{label}</Text>
      <Text style={styles.statValue}>{value}</Text>
    </View>
  );
}

const styles = StyleSheet.create({
  screen: { flex: 1, paddingHorizontal: 16, paddingTop: 12 },

  topBar:        { flexDirection: 'row', alignItems: 'center', marginBottom: 16 },
  backBtn:       { paddingVertical: 6, paddingRight: 12 },
  backBtnText:   { color: Colors.accent, fontSize: 12, fontFamily: 'monospace', letterSpacing: 2, fontWeight: '700' },
  pageTitle:     { flex: 1, textAlign: 'center', color: Colors.textPrimary, fontSize: 14, fontFamily: 'monospace', letterSpacing: 4, fontWeight: '800' },
  topBarRight:   { minWidth: 40, alignItems: 'flex-end' },
  countText:     { color: Colors.textSecondary, fontSize: 12, fontFamily: 'monospace', letterSpacing: 2 },

  // Lifetime stats card
  lifetimeCard:   { backgroundColor: Colors.surface, borderRadius: 4, padding: 14, marginBottom: 16, borderLeftWidth: 2, borderLeftColor: Colors.accent },
  lifetimeTitle:  { color: Colors.accent, fontSize: 10, letterSpacing: 4, fontFamily: 'monospace', fontWeight: '800', marginBottom: 10 },
  lifetimeRow:    { flexDirection: 'row', marginBottom: 6 },
  stat:           { flex: 1 },
  statLabel:      { color: Colors.textSecondary, fontSize: 9, letterSpacing: 2, fontFamily: 'monospace', marginBottom: 2 },
  statValue:      { color: Colors.textPrimary, fontSize: 15, fontFamily: 'monospace', fontWeight: '800' },

  row:           { flexDirection: 'row', alignItems: 'center', backgroundColor: Colors.surface, borderRadius: 4, paddingVertical: 12, paddingHorizontal: 14, marginBottom: 8 },
  rowTimestamp:  { color: Colors.textPrimary, fontSize: 14, fontFamily: 'monospace', fontWeight: '700' },
  rowMeta:       { color: Colors.textSecondary, fontSize: 11, fontFamily: 'monospace', marginTop: 3, letterSpacing: 1 },
  deleteBtn:     { paddingHorizontal: 10, paddingVertical: 4 },
  deleteBtnText: { color: Colors.danger, fontSize: 22, fontWeight: '700' },

  empty:           { color: Colors.textSecondary, fontSize: 12, fontFamily: 'monospace', textAlign: 'center', lineHeight: 18, paddingHorizontal: 20 },
  emptyContainer:  { flexGrow: 1, justifyContent: 'center' },
});
