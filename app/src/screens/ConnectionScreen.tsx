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
  const fastCandidates = [...new Set([lastIP, '192.168.4.1'].filter(Boolean) as string[])];
  onProgress('Probing known addresses…');
  const fast = await raceToSuccess(fastCandidates.map((ip) => probeIP(ip, 1500)));
  if (fast) return fast;

  onProgress('Scanning subnet…');
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

        {/* ── Wordmark ─────────────────────────────────────── */}
        <View style={styles.brandBlock}>
          <Text style={styles.brand}>LEGEND CUTTER</Text>
          <View style={styles.stripe}>
            <View style={[styles.stripeSeg, { backgroundColor: '#c8102e' }]} />
            <View style={[styles.stripeSeg, { backgroundColor: '#ffffff' }]} />
            <View style={[styles.stripeSeg, { backgroundColor: '#003a70' }]} />
          </View>
          <Text style={styles.subtitle}>AUTONOMOUS MARITIME PLATFORM</Text>
        </View>

        {/* ── Console-style status line ────────────────────── */}
        <View style={styles.consoleBlock}>
          <Text style={styles.consoleLine}>
            <Text style={styles.consolePrompt}>{'> '}</Text>
            {statusLine(state, scanMsg)}
          </Text>
        </View>

        {/* ── IP input ─────────────────────────────────────── */}
        <Text style={styles.fieldLabel}>BOAT IP</Text>
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
        {lastIP && lastIP !== ip && !busy && (
          <TouchableOpacity onPress={() => setIP(lastIP)}>
            <Text style={styles.lastIpHint}>↺ last connected: {lastIP}</Text>
          </TouchableOpacity>
        )}

        {/* ── Buttons ──────────────────────────────────────── */}
        <View style={styles.btnRow}>
          <TouchableOpacity
            style={[styles.btn, styles.btnSecondary, busy && styles.btnDisabled]}
            onPress={handleScan}
            disabled={busy}
            activeOpacity={0.7}
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
            activeOpacity={0.7}
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

function statusLine(state: ScreenState, scanMsg: string): string {
  if (state === 'scanning')   return scanMsg || 'Scanning…';
  if (state === 'connecting') return 'Connecting…';
  if (state === 'failed')     return scanMsg || 'Connection failed — verify IP and retry';
  return 'Ready. Confirm boat is powered and on the same WiFi.';
}

const styles = StyleSheet.create({
  inner:          { flex: 1, justifyContent: 'center', paddingHorizontal: 28 },

  brandBlock:     { alignItems: 'center', marginBottom: 36 },
  brand:          { color: Colors.textPrimary, fontSize: 26, fontWeight: '800', letterSpacing: 6, fontFamily: 'monospace' },
  stripe:         { flexDirection: 'row', width: 180, height: 4, marginTop: 8, borderRadius: 1, overflow: 'hidden' },
  stripeSeg:      { flex: 1, height: 4 },
  subtitle:       { color: Colors.textSecondary, fontSize: 10, letterSpacing: 3, fontFamily: 'monospace', marginTop: 12 },

  consoleBlock:   { backgroundColor: Colors.surface, paddingHorizontal: 12, paddingVertical: 10, borderRadius: 4, borderLeftWidth: 2, borderLeftColor: Colors.accent, marginBottom: 24 },
  consoleLine:    { color: Colors.textPrimary, fontSize: 12, fontFamily: 'monospace', lineHeight: 16 },
  consolePrompt:  { color: Colors.accent, fontWeight: '800' },

  fieldLabel:     { color: Colors.textSecondary, fontSize: 10, letterSpacing: 2, fontFamily: 'monospace', marginBottom: 6 },
  input:          { backgroundColor: Colors.surface, color: Colors.textPrimary, padding: 14, borderRadius: 4, fontSize: 16, fontFamily: 'monospace', letterSpacing: 1 },
  lastIpHint:     { color: Colors.accent, fontSize: 11, fontFamily: 'monospace', marginTop: 8, letterSpacing: 1 },

  btnRow:         { flexDirection: 'row', gap: 10, marginTop: 28 },
  btn:            { flex: 1, paddingVertical: 16, borderRadius: 4, alignItems: 'center' },
  btnPrimary:     { backgroundColor: Colors.accent },
  btnSecondary:   { backgroundColor: Colors.surface, borderWidth: 1, borderColor: Colors.accent },
  btnPrimaryText: { color: '#000', fontWeight: '800', fontSize: 13, letterSpacing: 3, fontFamily: 'monospace' },
  btnSecondaryText:{ color: Colors.accent, fontWeight: '800', fontSize: 13, letterSpacing: 3, fontFamily: 'monospace' },
  btnDisabled:    { opacity: 0.4 },
});
