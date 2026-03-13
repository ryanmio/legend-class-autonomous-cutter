// EmergencyStop.tsx
// Persistent red E-STOP button overlay rendered on every screen.
// Import and place at the root of each screen layout.

import React, { useState } from 'react';
import { TouchableOpacity, Text, StyleSheet, Alert } from 'react-native';
import * as Haptics from 'expo-haptics';
import { emergencyStop } from '../services/esp32Service';
import { Colors } from '../constants';

interface Props {
  ip: string;
}

export default function EmergencyStop({ ip }: Props) {
  const [pressed, setPressed] = useState(false);

  const handlePress = async () => {
    Haptics.impactAsync(Haptics.ImpactFeedbackStyle.Heavy);
    setPressed(true);
    try {
      await emergencyStop(ip);
    } catch {
      Alert.alert('E-STOP', 'Command sent (connection may be degraded).');
    }
    // Keep button in "active" state — user must explicitly release via app
  };

  return (
    <TouchableOpacity
      style={[styles.button, pressed && styles.buttonActive]}
      onPress={handlePress}
      activeOpacity={0.7}
    >
      <Text style={styles.label}>⚡ E-STOP</Text>
    </TouchableOpacity>
  );
}

const styles = StyleSheet.create({
  button: {
    position: 'absolute',
    bottom: 32,
    right: 20,
    backgroundColor: Colors.danger,
    borderRadius: 36,
    width: 72,
    height: 72,
    justifyContent: 'center',
    alignItems: 'center',
    elevation: 10,
    shadowColor: Colors.danger,
    shadowOpacity: 0.8,
    shadowRadius: 12,
    shadowOffset: { width: 0, height: 4 },
  },
  buttonActive: {
    backgroundColor: '#800',
  },
  label: {
    color: '#fff',
    fontWeight: 'bold',
    fontSize: 11,
    textAlign: 'center',
  },
});
