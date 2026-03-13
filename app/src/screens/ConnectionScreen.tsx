// ConnectionScreen.tsx
// Entry screen: user enters boat IP (default 192.168.4.1) and connects.
// On success, opens HelmScreen with the confirmed IP.

import React, { useState, useEffect } from 'react';
import { View, Text, TextInput, TouchableOpacity, StyleSheet, ActivityIndicator } from 'react-native';
import { NativeStackScreenProps } from '@react-navigation/native-stack';
import { RootStackParamList } from '../../App';
import { Colors, DEFAULT_BOAT_IP, DEFAULT_CAMERA_IP } from '../constants';
import { checkStatus } from '../services/esp32Service';
import { connect } from '../services/websocketService';
import { saveLastIP, loadLastIP } from '../services/storageService';

type Props = NativeStackScreenProps<RootStackParamList, 'Connection'>;

export default function ConnectionScreen({ navigation }: Props) {
  const [ip, setIP]         = useState(DEFAULT_BOAT_IP);
  const [camIP, setCamIP]   = useState(DEFAULT_CAMERA_IP);
  const [status, setStatus] = useState<'idle' | 'connecting' | 'failed'>('idle');

  useEffect(() => {
    loadLastIP().then((saved) => {
      if (saved) { setIP(saved.ip); setCamIP(saved.camIP); }
    });
  }, []);

  const handleConnect = async () => {
    setStatus('connecting');
    try {
      await checkStatus(ip);
      await saveLastIP(ip, camIP);
      connect(ip);
      navigation.replace('Helm', { ip, cameraIP: camIP });
    } catch {
      setStatus('failed');
    }
  };

  return (
    <View style={styles.container}>
      <Text style={styles.title}>LEGEND CUTTER</Text>
      <Text style={styles.subtitle}>AUTONOMOUS MARITIME PLATFORM</Text>

      <Text style={styles.label}>Boat IP</Text>
      <TextInput
        style={styles.input}
        value={ip}
        onChangeText={setIP}
        keyboardType="numeric"
        placeholder={DEFAULT_BOAT_IP}
        placeholderTextColor={Colors.textSecondary}
      />

      <Text style={styles.label}>Camera IP</Text>
      <TextInput
        style={styles.input}
        value={camIP}
        onChangeText={setCamIP}
        keyboardType="numeric"
        placeholder={DEFAULT_CAMERA_IP}
        placeholderTextColor={Colors.textSecondary}
      />

      {status === 'failed' && (
        <Text style={styles.error}>Connection failed — check WiFi and IP</Text>
      )}

      <TouchableOpacity style={styles.button} onPress={handleConnect} disabled={status === 'connecting'}>
        {status === 'connecting'
          ? <ActivityIndicator color="#fff" />
          : <Text style={styles.buttonText}>CONNECT</Text>
        }
      </TouchableOpacity>
    </View>
  );
}

const styles = StyleSheet.create({
  container:   { flex: 1, backgroundColor: Colors.background, justifyContent: 'center', padding: 32 },
  title:       { color: Colors.accent, fontSize: 28, fontWeight: 'bold', textAlign: 'center', letterSpacing: 4 },
  subtitle:    { color: Colors.textSecondary, fontSize: 11, textAlign: 'center', letterSpacing: 2, marginBottom: 48 },
  label:       { color: Colors.textSecondary, fontSize: 12, marginBottom: 4, marginTop: 16 },
  input:       { backgroundColor: Colors.surface, color: Colors.textPrimary, padding: 14, borderRadius: 8, fontSize: 16 },
  error:       { color: Colors.danger, textAlign: 'center', marginTop: 12 },
  button:      { backgroundColor: Colors.accent, padding: 16, borderRadius: 8, alignItems: 'center', marginTop: 32 },
  buttonText:  { color: '#fff', fontWeight: 'bold', fontSize: 16, letterSpacing: 2 },
});
