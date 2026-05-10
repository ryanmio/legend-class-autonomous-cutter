import React from 'react';
import { View, Text, Modal, Pressable, TouchableOpacity, StyleSheet } from 'react-native';
import { Colors } from '../constants';

// 1500 = neutral (static heading-hold). 1750 = firmware AUTO_CRUISE_CAP.
export const CRUISE_PRESETS = [1500, 1600, 1660, 1700, 1750];

export function CruiseModal({
  visible, currentUs, onPick, onCancel,
}: {
  visible: boolean;
  currentUs: number | undefined;
  onPick: (us: number) => void;
  onCancel: () => void;
}) {
  return (
    <Modal visible={visible} transparent animationType="fade" onRequestClose={onCancel}>
      <Pressable style={styles.backdrop} onPress={onCancel}>
        <Pressable style={styles.card}>
          <Text style={styles.title}>CRUISE</Text>
          <Text style={styles.current}>
            current: {currentUs != null ? `${currentUs} µs` : '—'}
          </Text>
          <Text style={styles.hint}>
            1500 = neutral (static heading-hold). 1750 = firmware cap.
          </Text>
          <View style={styles.row}>
            {CRUISE_PRESETS.map((us) => (
              <TouchableOpacity
                key={us}
                style={[styles.preset, currentUs === us && styles.presetActive]}
                onPress={() => onPick(us)}
              >
                <Text style={[styles.presetText, currentUs === us && styles.presetTextActive]}>
                  {us}
                </Text>
              </TouchableOpacity>
            ))}
          </View>
          <TouchableOpacity style={styles.cancel} onPress={onCancel}>
            <Text style={styles.cancelText}>CLOSE</Text>
          </TouchableOpacity>
        </Pressable>
      </Pressable>
    </Modal>
  );
}

const styles = StyleSheet.create({
  backdrop: {
    flex: 1, backgroundColor: 'rgba(0,0,0,0.6)',
    justifyContent: 'center', alignItems: 'center', padding: 24,
  },
  card: {
    backgroundColor: Colors.surface, borderRadius: 12, padding: 20,
    width: '100%', maxWidth: 400,
  },
  title: {
    color: Colors.accent, fontSize: 16, fontWeight: 'bold',
    letterSpacing: 2, marginBottom: 6, fontFamily: 'monospace',
  },
  current:   { color: Colors.textPrimary, fontSize: 14, fontFamily: 'monospace', marginBottom: 4 },
  hint:      { color: Colors.textSecondary, fontSize: 11, marginBottom: 14, lineHeight: 16 },
  row:       { flexDirection: 'row', gap: 6, justifyContent: 'space-between', marginBottom: 12 },
  preset:    { flex: 1, paddingVertical: 12, backgroundColor: Colors.surfaceLight, borderRadius: 6, alignItems: 'center' },
  presetActive:      { backgroundColor: Colors.accent },
  presetText:        { color: Colors.textPrimary, fontFamily: 'monospace', fontWeight: 'bold' },
  presetTextActive:  { color: '#000' },
  cancel:     { paddingVertical: 12, alignItems: 'center', marginTop: 4 },
  cancelText: { color: Colors.textSecondary, fontFamily: 'monospace', letterSpacing: 2 },
});
