import React, { useState, useEffect } from 'react';
import {
  Text, TextInput, TouchableOpacity,
  StyleSheet, ActivityIndicator, View,
} from 'react-native';
import { NativeStackScreenProps } from '@react-navigation/native-stack';
import { RootStackParamList } from '../../App';
import { Colors, DEFAULT_BOAT_IP, HTTP_PORT } from '../constants';
import { checkStatus } from '../services/esp32Service';
import { connect } from '../services/websocketService';
import { saveLastIP, loadLastIP } from '../services/storageService';
import Screen from '../components/Screen';

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

// First caller to resolve with a non-null value wins.
function raceToSuccess(promises: Promise<string | null>[]): Promise<string | null> {
  return new Promise((resolve) => {
    let remaining = promises.length;
    let done = false;
    for (const p of promises) {
      p.then((v) => {
        if (v && !done) { done = true; resolve(v); }
      }).finally(() => {
        if (--remaining === 0 && !done) resolve(null);
      });
    }
  });
}

async function scanNetwork(
  lastIP: string | null,
  onProgress: (msg: string) => void,
): Promise<string | null> {
  // Fast path: try last-used IP and AP default simultaneously
  const fastCandidates = [...new Set([lastIP, '192.168.4.1'].filter(Boolean) as string[])];
  onProgress(`Trying known addresses…`);
  const fast = await raceToSuccess(fastCandidates.map((ip) => probeIP(ip, 1500)));
  if (fast) return fast;

  // Full subnet scan — covers .1 through .254 on common home/hotspot subnets
  onProgress('Scanning network…');
  const subnets = ['192.168.1', '192.168.0', '10.0.0', '10.0.1', '172.20.10'];
  const candidates: string[] = [];
  for (const subnet of subnets) {
    for (let i = 1; i <= 254; i++) {
      const ip = `${subnet}.${i}`;
      if (!fastCandidates.includes(ip)) candidates.push(ip);
    }
  }

  return raceToSuccess(candidates.map((ip) => probeIP(ip, 1500)));
}

// ── Screen ────────────────────────────────────────────────────────────────────

export default function ConnectionScreen({ navigation }: Props) {
  const [ip, setIP]           = useState(DEFAULT_BOAT_IP);
  const [lastIP, setLastIP]   = useState<string | null>(null);
  const [state, setState]     = useState<ScreenState>('idle');
  const [scanMsg, setScanMsg] = useState('');

  useEffect(() => {
    loadLastIP().then((saved) => {
      if (saved) { setIP(saved); setLastIP(saved); }
    });
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
      setScanMsg('');
    }
  };

  const handleScan = async () => {
    setState('scanning');
    setScanMsg('');
    const found = await scanNetwork(lastIP, setScanMsg);
    if (found) {
      setIP(found);
      await doConnect(found);
    } else {
      setScanMsg('No boat found — check WiFi and that firmware is running');
      setState('failed');
    }
  };

  const busy = state === 'scanning' || state === 'connecting';

  return (
    <Screen>
      <View style={styles.inner}>
        <Text style={styles.title}>LEGEND CUTTER</Text>
        <Text style={styles.subtitle}>AUTONOMOUS MARITIME PLATFORM</Text>

        <Text style={styles.hint}>
          Make sure the boat is powered on and your phone is on the same WiFi.
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
          autoCorrect={false}
        />

        {(state === 'scanning' || (state === 'failed' && scanMsg)) ? (
          <Text style={[styles.msg, state === 'failed' && styles.msgError]}>{scanMsg}</Text>
        ) : state === 'failed' ? (
          <Text style={styles.msgError}>Connection failed — check IP and try again</Text>
        ) : null}

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
    </Screen>
  );
}

const styles = StyleSheet.create({
  inner:           { flex: 1, justifyContent: 'center', padding: 32 },
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
