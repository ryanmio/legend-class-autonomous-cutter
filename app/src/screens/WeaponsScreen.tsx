import React from 'react';
import { View, Text, StyleSheet } from 'react-native';
import { NativeStackScreenProps } from '@react-navigation/native-stack';
import { RootStackParamList } from '../../App';
import { Colors } from '../constants';
import Screen from '../components/Screen';

type Props = NativeStackScreenProps<RootStackParamList, 'Weapons'>;

// Placeholder. Deck gun + CIWS + audio controls will land here once
// production firmware exposes the endpoints; intentionally empty until
// then so we don't ship dead buttons.
export default function WeaponsScreen(_: Props) {
  return (
    <Screen>
      <View style={styles.inner}>
        <Text style={styles.title}>WEAPONS CONTROL</Text>
        <Text style={styles.msg}>Controls land when firmware is ready.</Text>
      </View>
    </Screen>
  );
}

const styles = StyleSheet.create({
  inner: { flex: 1, justifyContent: 'center', alignItems: 'center', padding: 16 },
  title: { color: Colors.accent, fontSize: 18, fontWeight: 'bold', marginBottom: 12, letterSpacing: 2 },
  msg:   { color: Colors.textSecondary, fontStyle: 'italic', textAlign: 'center' },
});
