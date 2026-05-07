// FPVScreen.tsx
// Placeholder — no camera ESP on this build.
// Screen is kept in the navigator for when a camera module is added later.

import React from 'react';
import { View, Text, StyleSheet } from 'react-native';
import { NativeStackScreenProps } from '@react-navigation/native-stack';
import { RootStackParamList } from '../../App';
import { Colors } from '../constants';
import EmergencyStop from '../components/EmergencyStop';

type Props = NativeStackScreenProps<RootStackParamList, 'FPV'>;

export default function FPVScreen({ route }: Props) {
  const { ip } = route.params;

  return (
    <View style={styles.container}>
      <Text style={styles.title}>FPV</Text>
      <Text style={styles.msg}>No camera module on this build.</Text>
      <EmergencyStop ip={ip} />
    </View>
  );
}

const styles = StyleSheet.create({
  container: { flex: 1, backgroundColor: '#000', justifyContent: 'center', alignItems: 'center' },
  title:     { color: Colors.accent, fontSize: 18, fontWeight: 'bold', marginBottom: 12 },
  msg:       { color: Colors.textSecondary, fontStyle: 'italic' },
});
