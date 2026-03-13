// SurveyScreen.tsx
// Survey planner: draw bounding box on map, set transect spacing,
// launch autonomous survey, monitor progress, export depth log as CSV.
// TODO (Phase 3+): implement lawnmower grid generation and progress tracking.

import React from 'react';
import { View, Text, TouchableOpacity, StyleSheet, Alert } from 'react-native';
import { NativeStackScreenProps } from '@react-navigation/native-stack';
import { RootStackParamList } from '../../App';
import { Colors } from '../constants';
import { loadDepthLog, depthLogToCSV, clearDepthLog } from '../services/storageService';
import EmergencyStop from '../components/EmergencyStop';

type Props = NativeStackScreenProps<RootStackParamList, 'Survey'>;

export default function SurveyScreen({ route }: Props) {
  const { ip } = route.params;

  const handleExport = async () => {
    const log = await loadDepthLog();
    if (log.length === 0) { Alert.alert('No data', 'No depth log recorded yet.'); return; }
    const csv = depthLogToCSV(log);
    // TODO: use expo-sharing to share CSV file
    Alert.alert('Export', `${log.length} depth points ready (share not yet wired up).`);
  };

  const handleClear = async () => {
    Alert.alert('Clear depth log?', 'This cannot be undone.', [
      { text: 'Cancel', style: 'cancel' },
      { text: 'Clear', style: 'destructive', onPress: clearDepthLog },
    ]);
  };

  return (
    <View style={styles.container}>
      <Text style={styles.title}>SURVEY PLANNER</Text>
      <Text style={styles.placeholder}>Bounding box editor + grid generator (Phase 3)</Text>

      <View style={styles.btnRow}>
        <TouchableOpacity style={styles.btn} onPress={handleExport}>
          <Text style={styles.btnText}>EXPORT CSV</Text>
        </TouchableOpacity>
        <TouchableOpacity style={[styles.btn, styles.btnDanger]} onPress={handleClear}>
          <Text style={styles.btnText}>CLEAR LOG</Text>
        </TouchableOpacity>
      </View>

      <EmergencyStop ip={ip} />
    </View>
  );
}

const styles = StyleSheet.create({
  container:   { flex: 1, backgroundColor: Colors.background, padding: 16 },
  title:       { color: Colors.accent, fontSize: 18, fontWeight: 'bold', marginBottom: 16 },
  placeholder: { color: Colors.textSecondary, fontStyle: 'italic', marginBottom: 32 },
  btnRow:      { flexDirection: 'row', gap: 12 },
  btn:         { backgroundColor: Colors.surface, padding: 14, borderRadius: 8, flex: 1, alignItems: 'center' },
  btnDanger:   { backgroundColor: '#3a1010' },
  btnText:     { color: Colors.textPrimary, fontWeight: 'bold', fontSize: 13 },
});
