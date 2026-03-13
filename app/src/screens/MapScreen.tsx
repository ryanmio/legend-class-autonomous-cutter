// MapScreen.tsx
// Real-time boat position on MapKit map, GPS track trail, waypoint pins,
// depth colour overlay (bathymetric), geofence polygon editor, survey grid drawing.
// TODO (Phase 2+): Implement MapView with react-native-maps.

import React from 'react';
import { View, Text, StyleSheet } from 'react-native';
import { NativeStackScreenProps } from '@react-navigation/native-stack';
import { RootStackParamList } from '../../App';
import { Colors } from '../constants';
import { useTelemetry } from '../hooks/useTelemetry';
import EmergencyStop from '../components/EmergencyStop';

type Props = NativeStackScreenProps<RootStackParamList, 'Map'>;

export default function MapScreen({ route }: Props) {
  const { ip } = route.params;
  const { data } = useTelemetry();

  return (
    <View style={styles.container}>
      <Text style={styles.title}>MAP</Text>
      <Text style={styles.coords}>
        {data?.gps_fix ? `${data.lat}, ${data.lon}` : 'No GPS fix'}
      </Text>
      <Text style={styles.placeholder}>MapView + track trail + waypoints (Phase 2)</Text>
      <EmergencyStop ip={ip} />
    </View>
  );
}

const styles = StyleSheet.create({
  container:   { flex: 1, backgroundColor: Colors.background, padding: 16 },
  title:       { color: Colors.accent, fontSize: 20, fontWeight: 'bold', marginBottom: 8 },
  coords:      { color: Colors.textPrimary, fontSize: 14, marginBottom: 16 },
  placeholder: { color: Colors.textSecondary, fontStyle: 'italic' },
});
