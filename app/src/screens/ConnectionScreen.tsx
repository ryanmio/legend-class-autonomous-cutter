// ConnectionScreen.tsx
// Enter boat IP or scan the network to discover it automatically.
// When connected to the boat's WiFi AP ("LegendCutter"), the IP is
// always 192.168.4.1. Scan is useful when testing at home on a router.

import React, { useState, useEffect } from 'react';
import {
  View, Text, TextInput, TouchableOpacity,
  StyleSheet, ActivityIndicator,
} from 'react-native';
import { NativeStackScreenProps } from '@react-navigation/native-stack';
import { RootStackParamList } from '../../App';
import { Colors, DEFAULT_BOAT_IP, HTTP_PORT } from '../constants';
import { checkStatus } from '../services/esp32Service';
import { connect } from '../services/websocketService';
import { saveLastIP, loadLastIP } from '../services/storageService';

type Props = NativeStackScreenProps<RootStackParamList, 'Connection'>;
type ScreenState = 'idle' | 'scanning' | 'connecting' | 'failed';

// ── Scan helpers ──────────────────────────────────────────────────────────────

async function probeIP(ip: string, timeoutMs: number): Promise<string | null> {
  try {
    const controller = new AbortController();
    const timer = setTimeout(() => controller.abort(), timeoutMs);
    const res = await fetch(`http://${ip}:${HTTP_PORT}/status`, {
      signal: controller.signal,
    });
    clearTimeout(timer);
    if (res.ok) {
      const data = await res.json();
      if (data?.ok) return ip;
    }
  } catch {}
  return null;
}

// Returns the first IP that answers /status with {ok:true}.
// Tries the boat AP address first (fast path), then common subnets in parallel.
async function scanNetwork(
  onProgress: (msg: string) => void
): Promise<string | null> {
  onProgress('Trying 192.168.4.1…');
  const fast = await probeIP('192.168.4.1', 1200);
  if (fast) return fast;

  onProgress('Scanning network…');

  const candidates: string[] = [];
  for (const subnet of ['192.168.1', '192.168.0', '172.20.10']) {
    for (let i = 1; i <= 30; i++) candidates.push(`${subnet}.${i}`);
  }

  return new Promise((resolve) => {
    let remaining = candidates.length;
    let done = false;
    for (const ip of candidates) {
      probeIP(ip, 1500).then((result) => {
        if (result && !done) { done = true; resolve(result); }
        if (--remaining === 0 && !done) resolve(null);
      });
    }
  });
}

// ── Screen ────────────────────────────────────────────────────────────────────

export default function ConnectionScreen({ navigation }: Props) {
  const [ip, setIP]           = useState(DEFAULT_BOAT_IP);
  const [state, setState]     = useState<ScreenState>('idle');
  const [scanMsg, setScanMsg] = useState('');

  useEffect(() => {
    loadLastIP().then((saved) => { if (saved) setIP(saved); });
  }, []);

  const doConnect = async (targetIP: string) => {
    setState('connecting');
    try {
      await checkStatus(targetIP);
      await saveLastIP(targetIP);
      connect(targetIP);
      navigation.replace('Helm', { ip: targetIP });
    } catch {
      setState('failed');
    }
  };

  const handleScan = async () => {
    setState('scanning');
    setScanMsg('');
    const found = await scanNetwork(setScanMsg);
    if (found) {
      setIP(found);
      await doConnect(found);
    } else {
      setScanMsg('No boat found — check WiFi');
      setState('failed');
    }
  };

  const busy = state === 'scanning' || state === 'connecting';

  return (
    <View style={styles.container}>
      <Text style={styles.title}>LEGEND CUTTER</Text>
      <Text style={styles.subtitle}>AUTONOMOUS MARITIME PLATFORM</Text>

      <Text style={styles.hint}>
        Connect phone to "LegendCutter" WiFi, then tap SCAN or CONNECT.
      </Text>

      <Text style={styles.label}>Boat IP</Text>
      <TextInput
        style={styles.input}
        value={ip}
        onChangeText={setIP}
        keyboardType="numeric"
        placeholder={DEFAULT_BOAT_IP}
        placeholderTextColor={Colors.textSecondary}
        editable={!busy}
      />

      {(state === 'failed' || state === 'scanning') && scanMsg !== '' && (
        <Text style={[styles.msg, state === 'failed' && styles.msgError]}>
          {scanMsg}
        </Text>
      )}
      {state === 'failed' && scanMsg === '' && (
        <Text style={styles.msgError}>Connection failed — check WiFi and IP</Text>
      )}

      <View style={styles.btnRow}>
        <TouchableOpacity
          style={[styles.btn, styles.btnSecondary, busy && styles.btnDisabled]}
          onPress={handleScan}
          disabled={busy}
        >
          {state === 'scanning'
            ? <ActivityIndicator color={Colors.accent} />
            : <Text style={styles.btnSecondaryText}>SCAN</Text>
          }
        </TouchableOpacity>

        <TouchableOpacity
          style={[styles.btn, styles.btnPrimary, busy && styles.btnDisabled]}
          onPress={() => doConnect(ip.trim())}
          disabled={busy}
        >
          {state === 'connecting'
            ? <ActivityIndicator color="#fff" />
            : <Text style={styles.btnPrimaryText}>CONNECT</Text>
          }
        </TouchableOpacity>
      </View>
    </View>
  );
}

const styles = StyleSheet.create({
  container:       { flex: 1, backgroundColor: Colors.background, justifyContent: 'center', padding: 32 },
  title:           { color: Colors.accent, fontSize: 28, fontWeight: 'bold', textAlign: 'center', letterSpacing: 4 },
  subtitle:        { color: Colors.textSecondary, fontSize: 11, textAlign: 'center', letterSpacing: 2, marginBottom: 40 },
  hint:            { color: Colors.textSecondary, fontSize: 12, textAlign: 'center', marginBottom: 24, lineHeight: 18 },
  label:           { color: Colors.textSecondary, fontSize: 12, marginBottom: 4 },
  input:           { backgroundColor: Colors.surface, color: Colors.textPrimary, padding: 14, borderRadius: 8, fontSize: 16 },
  msg:             { color: Colors.textSecondary, textAlign: 'center', marginTop: 10, fontSize: 13 },
  msgError:        { color: Colors.danger, textAlign: 'center', marginTop: 10, fontSize: 13 },
  btnRow:          { flexDirection: 'row', gap: 12, marginTop: 28 },
  btn:             { flex: 1, padding: 16, borderRadius: 8, alignItems: 'center' },
  btnPrimary:      { backgroundColor: Colors.accent },
  btnSecondary:    { backgroundColor: Colors.surface, borderWidth: 1, borderColor: Colors.accent },
  btnPrimaryText:  { color: '#fff', fontWeight: 'bold', fontSize: 15, letterSpacing: 2 },
  btnSecondaryText:{ color: Colors.accent, fontWeight: 'bold', fontSize: 15, letterSpacing: 2 },
  btnDisabled:     { opacity: 0.5 },
});
