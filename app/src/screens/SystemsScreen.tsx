import React, { useState } from 'react';
import { View, Text, TouchableOpacity, ScrollView, StyleSheet } from 'react-native';
import { NativeStackScreenProps } from '@react-navigation/native-stack';
import { RootStackParamList } from '../../App';
import { Colors } from '../constants';
import { setBayDoor, setAnchor, setLed } from '../services/esp32Service';
import { useTelemetry } from '../hooks/useTelemetry';
import Screen from '../components/Screen';

type Props = NativeStackScreenProps<RootStackParamList, 'Systems'>;

export default function SystemsScreen({ route }: Props) {
  const { ip } = route.params;
  const { data } = useTelemetry();

  return (
    <Screen>
      <Text style={styles.title}>SYSTEMS</Text>
      <ScrollView contentContainerStyle={styles.scroll}>

        <Text style={styles.section}>LIGHTS</Text>
        <View style={styles.row}>
          {([
            { key: 'nav'    as const, label: 'NAV',    on: data?.nav_on    ?? false },
            { key: 'bridge' as const, label: 'BRIDGE', on: data?.bridge_on ?? false },
            { key: 'deck'   as const, label: 'DECK',   on: data?.deck_on   ?? false },
          ]).map(({ key, label, on }) => (
            <TouchableOpacity
              key={key}
              style={[styles.toggleBtn, on && styles.toggleOn]}
              onPress={() => setLed(ip, key, !on).catch(() => {})}
            >
              <Text style={[styles.toggleText, on && styles.toggleTextOn]}>{on ? `${label} ON` : label}</Text>
            </TouchableOpacity>
          ))}
        </View>

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

      </ScrollView>
    </Screen>
  );
}

const styles = StyleSheet.create({
  title:       { color: Colors.accent, fontSize: 18, fontWeight: 'bold', paddingHorizontal: 16, paddingTop: 16, marginBottom: 4 },
  section:     { color: Colors.textSecondary, fontSize: 11, letterSpacing: 2, marginTop: 20, marginBottom: 8, paddingHorizontal: 16 },
  scroll:      { paddingHorizontal: 16, paddingBottom: 32 },
  row:         { flexDirection: 'row', gap: 12 },
  group:       { flex: 1 },
  groupLabel:  { color: Colors.textSecondary, fontSize: 11, marginBottom: 4 },
  btn:         { backgroundColor: Colors.surface, padding: 10, borderRadius: 6, marginBottom: 4, alignItems: 'center' },
  btnText:     { color: Colors.textPrimary, fontSize: 12, fontWeight: '600' },
  toggleBtn:   { flex: 1, backgroundColor: Colors.surface, padding: 18, borderRadius: 8, alignItems: 'center' },
  toggleOn:    { backgroundColor: Colors.accent },
  toggleText:  { color: Colors.textSecondary, fontWeight: 'bold', fontSize: 13, letterSpacing: 1 },
  toggleTextOn:{ color: '#000' },
});
