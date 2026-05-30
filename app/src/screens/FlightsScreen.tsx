// FlightsScreen — lists saved flights from AsyncStorage. Tap to share
// the CSV via the system Share sheet; × to delete.

import React, { useCallback, useEffect, useState } from 'react';
import {
  View, Text, StyleSheet, TouchableOpacity, FlatList, Alert, Share, RefreshControl,
} from 'react-native';
import { NativeStackScreenProps } from '@react-navigation/native-stack';
import { RootStackParamList } from '../../App';
import { Colors } from '../constants';
import Screen from '../components/Screen';
import {
  FlightMeta, listFlights, loadFlightCSV, deleteFlight,
} from '../services/telemetryLogger';

type Props = NativeStackScreenProps<RootStackParamList, 'Flights'>;

function fmtTimestamp(ms: number): string {
  const d = new Date(ms);
  const pad = (n: number) => String(n).padStart(2, '0');
  return `${d.getFullYear()}-${pad(d.getMonth() + 1)}-${pad(d.getDate())} ${pad(d.getHours())}:${pad(d.getMinutes())}`;
}

function fmtDuration(ms: number): string {
  const totalSec = Math.max(0, Math.round(ms / 1000));
  const m = Math.floor(totalSec / 60);
  const s = totalSec % 60;
  if (m === 0) return `${s}s`;
  return `${m}m ${String(s).padStart(2, '0')}s`;
}

export default function FlightsScreen({ navigation }: Props) {
  const [flights, setFlights]   = useState<FlightMeta[]>([]);
  const [refreshing, setRefresh] = useState(false);

  const load = useCallback(async () => {
    const list = await listFlights();
    // Newest first.
    list.sort((a, b) => b.startTs - a.startTs);
    setFlights(list);
  }, []);

  useEffect(() => { load(); }, [load]);

  const onRefresh = useCallback(async () => {
    setRefresh(true);
    await load();
    setRefresh(false);
  }, [load]);

  const onShare = useCallback(async (meta: FlightMeta) => {
    const csv = await loadFlightCSV(meta.id);
    if (!csv) { Alert.alert('Flight missing', 'CSV body not found in storage.'); return; }
    await Share.share({
      title:   `legend-cutter-flight-${meta.id}.csv`,
      message: csv,
    });
  }, []);

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
          refreshControl={
            <RefreshControl refreshing={refreshing} onRefresh={onRefresh} tintColor={Colors.accent} />
          }
          ListEmptyComponent={
            <Text style={styles.empty}>
              No flights recorded yet.{'\n\n'}
              Logging starts automatically when the boat is throttled up
              and ends when telemetry is lost for 60 s, the boat reboots,
              or you tap STOP on Telemetry.
            </Text>
          }
          contentContainerStyle={flights.length === 0 ? styles.emptyContainer : undefined}
          renderItem={({ item }) => (
            <TouchableOpacity
              style={styles.row}
              activeOpacity={0.7}
              onPress={() => onShare(item)}
              onLongPress={() => onDelete(item)}
            >
              <View style={{ flex: 1 }}>
                <Text style={styles.rowTimestamp}>{fmtTimestamp(item.startTs)}</Text>
                <Text style={styles.rowMeta}>
                  {fmtDuration(item.endTs - item.startTs)} · {item.rowCount} row{item.rowCount === 1 ? '' : 's'}
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

const styles = StyleSheet.create({
  screen: { flex: 1, paddingHorizontal: 16, paddingTop: 12 },

  topBar:        { flexDirection: 'row', alignItems: 'center', marginBottom: 16 },
  backBtn:       { paddingVertical: 6, paddingRight: 12 },
  backBtnText:   { color: Colors.accent, fontSize: 12, fontFamily: 'monospace', letterSpacing: 2, fontWeight: '700' },
  pageTitle:     { flex: 1, textAlign: 'center', color: Colors.textPrimary, fontSize: 14, fontFamily: 'monospace', letterSpacing: 4, fontWeight: '800' },
  topBarRight:   { minWidth: 40, alignItems: 'flex-end' },
  countText:     { color: Colors.textSecondary, fontSize: 12, fontFamily: 'monospace', letterSpacing: 2 },

  row:           { flexDirection: 'row', alignItems: 'center', backgroundColor: Colors.surface, borderRadius: 4, paddingVertical: 12, paddingHorizontal: 14, marginBottom: 8 },
  rowTimestamp:  { color: Colors.textPrimary, fontSize: 14, fontFamily: 'monospace', fontWeight: '700' },
  rowMeta:       { color: Colors.textSecondary, fontSize: 11, fontFamily: 'monospace', marginTop: 3, letterSpacing: 1 },
  deleteBtn:     { paddingHorizontal: 10, paddingVertical: 4 },
  deleteBtnText: { color: Colors.danger, fontSize: 22, fontWeight: '700' },

  empty:           { color: Colors.textSecondary, fontSize: 12, fontFamily: 'monospace', textAlign: 'center', lineHeight: 18, paddingHorizontal: 20 },
  emptyContainer:  { flexGrow: 1, justifyContent: 'center' },
});
