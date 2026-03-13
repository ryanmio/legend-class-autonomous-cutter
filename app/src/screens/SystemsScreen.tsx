// SystemsScreen.tsx
// Bay doors open/close, anchor deploy/raise, radar on/off, sound effects library.

import React, { useState } from 'react';
import { View, Text, TouchableOpacity, StyleSheet } from 'react-native';
import { NativeStackScreenProps } from '@react-navigation/native-stack';
import { RootStackParamList } from '../../App';
import { Colors } from '../constants';
import { setBayDoor, setAnchor, setRadar } from '../services/esp32Service';
import EmergencyStop from '../components/EmergencyStop';

type Props = NativeStackScreenProps<RootStackParamList, 'Systems'>;

export default function SystemsScreen({ route }: Props) {
  const { ip } = route.params;
  const [radar, setRadarState] = useState(false);

  const toggleRadar = async () => {
    const next = !radar;
    setRadarState(next);
    await setRadar(ip, next).catch(() => {});
  };

  return (
    <View style={styles.container}>
      <Text style={styles.title}>SYSTEMS</Text>

      <Text style={styles.section}>BAY DOORS</Text>
      <View style={styles.row}>
        {(['port', 'stbd'] as const).map((side) => (
          <View key={side} style={styles.group}>
            <Text style={styles.groupLabel}>{side.toUpperCase()}</Text>
            {(['open', 'close', 'stop'] as const).map((action) => (
              <TouchableOpacity key={action} style={styles.btn} onPress={() => setBayDoor(ip, side, action)}>
                <Text style={styles.btnText}>{action.toUpperCase()}</Text>
              </TouchableOpacity>
            ))}
          </View>
        ))}
      </View>

      <Text style={styles.section}>ANCHORS</Text>
      <View style={styles.row}>
        {(['fwd', 'aft'] as const).map((which) => (
          <View key={which} style={styles.group}>
            <Text style={styles.groupLabel}>{which.toUpperCase()}</Text>
            {(['lower', 'raise', 'stop'] as const).map((action) => (
              <TouchableOpacity key={action} style={styles.btn} onPress={() => setAnchor(ip, which, action)}>
                <Text style={styles.btnText}>{action.toUpperCase()}</Text>
              </TouchableOpacity>
            ))}
          </View>
        ))}
      </View>

      <Text style={styles.section}>RADAR</Text>
      <TouchableOpacity style={[styles.toggleBtn, radar && styles.toggleOn]} onPress={toggleRadar}>
        <Text style={styles.toggleText}>{radar ? 'ROTATING' : 'STOPPED'}</Text>
      </TouchableOpacity>

      <EmergencyStop ip={ip} />
    </View>
  );
}

const styles = StyleSheet.create({
  container:  { flex: 1, backgroundColor: Colors.background, padding: 16 },
  title:      { color: Colors.accent, fontSize: 18, fontWeight: 'bold', marginBottom: 16 },
  section:    { color: Colors.textSecondary, fontSize: 11, letterSpacing: 2, marginTop: 20, marginBottom: 8 },
  row:        { flexDirection: 'row', gap: 16 },
  group:      { flex: 1 },
  groupLabel: { color: Colors.textSecondary, fontSize: 11, marginBottom: 4 },
  btn:        { backgroundColor: Colors.surface, padding: 10, borderRadius: 6, marginBottom: 4, alignItems: 'center' },
  btnText:    { color: Colors.textPrimary, fontSize: 12, fontWeight: '600' },
  toggleBtn:  { backgroundColor: Colors.surface, padding: 14, borderRadius: 8, alignItems: 'center', width: 140 },
  toggleOn:   { backgroundColor: Colors.accent },
  toggleText: { color: Colors.textPrimary, fontWeight: 'bold' },
});
