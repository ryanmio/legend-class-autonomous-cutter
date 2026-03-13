// WeaponsScreen.tsx
// Virtual joystick for deck gun pan/tilt, Phalanx controls,
// animation mode selector, LRAD trigger buttons.
// TODO (Phase 4): wire up setGunPosition / setCIWS / setAnimMode calls.

import React, { useState } from 'react';
import { View, Text, TouchableOpacity, StyleSheet } from 'react-native';
import { NativeStackScreenProps } from '@react-navigation/native-stack';
import { RootStackParamList } from '../../App';
import { Colors } from '../constants';
import { AnimMode } from '../types';
import { setAnimMode, playAudio } from '../services/esp32Service';
import EmergencyStop from '../components/EmergencyStop';

type Props = NativeStackScreenProps<RootStackParamList, 'Weapons'>;

const ANIM_MODES: { key: AnimMode; label: string }[] = [
  { key: 'none',         label: 'SAFE' },
  { key: 'patrol_scan',  label: 'PATROL SCAN' },
  { key: 'track_target', label: 'TRACK TARGET' },
  { key: 'combat_demo',  label: 'COMBAT DEMO' },
  { key: 'random_alert', label: 'RANDOM ALERT' },
  { key: 'lrad_hail',    label: 'LRAD HAIL' },
];

export default function WeaponsScreen({ route }: Props) {
  const { ip } = route.params;
  const [activeMode, setActiveMode] = useState<AnimMode>('none');

  const handleMode = async (mode: AnimMode) => {
    setActiveMode(mode);
    await setAnimMode(ip, mode).catch(() => {});
  };

  return (
    <View style={styles.container}>
      <Text style={styles.title}>WEAPONS CONTROL</Text>

      <Text style={styles.section}>ANIMATION MODE</Text>
      <View style={styles.modeGrid}>
        {ANIM_MODES.map(({ key, label }) => (
          <TouchableOpacity
            key={key}
            style={[styles.modeBtn, activeMode === key && styles.modeBtnActive]}
            onPress={() => handleMode(key)}
          >
            <Text style={[styles.modeBtnText, activeMode === key && { color: '#000' }]}>{label}</Text>
          </TouchableOpacity>
        ))}
      </View>

      <Text style={styles.section}>SOUND EFFECTS</Text>
      <View style={styles.audioRow}>
        {[['HORN', 3], ['GUN FIRE', 4], ['CIWS BRRT', 5], ['ALARM', 6]].map(([label, track]) => (
          <TouchableOpacity key={label} style={styles.audioBtn} onPress={() => playAudio(ip, track as number)}>
            <Text style={styles.audioBtnText}>{label as string}</Text>
          </TouchableOpacity>
        ))}
      </View>

      <Text style={styles.placeholder}>Deck gun + CIWS joysticks (Phase 4)</Text>

      <EmergencyStop ip={ip} />
    </View>
  );
}

const styles = StyleSheet.create({
  container:      { flex: 1, backgroundColor: Colors.background, padding: 16 },
  title:          { color: Colors.accent, fontSize: 18, fontWeight: 'bold', marginBottom: 16 },
  section:        { color: Colors.textSecondary, fontSize: 11, letterSpacing: 2, marginBottom: 8, marginTop: 16 },
  modeGrid:       { flexDirection: 'row', flexWrap: 'wrap', gap: 8 },
  modeBtn:        { backgroundColor: Colors.surface, paddingVertical: 10, paddingHorizontal: 14, borderRadius: 6 },
  modeBtnActive:  { backgroundColor: Colors.accent },
  modeBtnText:    { color: Colors.textPrimary, fontSize: 12, fontWeight: 'bold' },
  audioRow:       { flexDirection: 'row', gap: 8, flexWrap: 'wrap' },
  audioBtn:       { backgroundColor: Colors.surfaceLight, paddingVertical: 10, paddingHorizontal: 14, borderRadius: 6 },
  audioBtnText:   { color: Colors.textPrimary, fontSize: 12 },
  placeholder:    { color: Colors.textSecondary, fontStyle: 'italic', marginTop: 32, textAlign: 'center' },
});
