import React, { useState, useEffect, useRef } from 'react';
import {
  Text, TextInput, TouchableOpacity, StyleSheet, ActivityIndicator,
  View, Animated, Pressable, Keyboard,
} from 'react-native';
import { NativeStackScreenProps } from '@react-navigation/native-stack';
import { RootStackParamList } from '../../App';
import { Colors, DEFAULT_BOAT_IP, HTTP_PORT } from '../constants';
import { checkStatus } from '../services/esp32Service';
import { connect } from '../services/websocketService';
import { promptRecoverable } from '../services/flightlogService';
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
  const [advancedOpen, setAdvancedOpen] = useState(false);

  // Sweep-bar animation for SCAN. ~700 ms per cycle, loops while scanning.
  const sweep = useRef(new Animated.Value(0)).current;

  useEffect(() => {
    loadLastIP().then((saved) => {
      if (saved) { setIP(saved); setLastIP(saved); }
    });
  }, []);

  useEffect(() => {
    if (state === 'scanning') {
      sweep.setValue(0);
      const loop = Animated.loop(
        Animated.timing(sweep, {
          toValue: 1,
          duration: 700,
          useNativeDriver: false,
        })
      );
      loop.start();
      return () => loop.stop();
    } else {
      sweep.setValue(0);
    }
  }, [state, sweep]);

  const doConnect = async (targetIP: string) => {
    Keyboard.dismiss();
    setState('connecting');
    try {
      await checkStatus(targetIP);
      await saveLastIP(targetIP);
      connect(targetIP);
      // Fire-and-forget: if the boat's flash log holds missions from previous
      // boots (crash / field power-off), surface a one-time pointer to the
      // FLIGHT LOGS import flow. Never blocks or fails the connect.
      void promptRecoverable(targetIP);
      navigation.replace('Helm', { ip: targetIP });
    } catch {
      setState('failed');
      setScanMsg('');
    }
  };

  const handleScan = async () => {
    Keyboard.dismiss();
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

  const busy        = state === 'scanning' || state === 'connecting';
  const showConsole = state !== 'idle';
  const consoleMsg  = state === 'scanning'   ? (scanMsg || 'Scanning…')
                    : state === 'connecting' ? 'Connecting…'
                    : state === 'failed'     ? (scanMsg || 'Connection failed — verify IP and retry')
                    : '';
  const consoleColor = state === 'failed' ? Colors.danger : Colors.textPrimary;

  return (
    <Screen>
      <Pressable style={styles.inner} onPress={Keyboard.dismiss}>

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

        {/* ── Console block (only while busy / failed) ────── */}
        {showConsole && (
          <View style={styles.consoleBlock}>
            <Text style={[styles.consoleLine, { color: consoleColor }]}>
              <Text style={styles.consolePrompt}>{'> '}</Text>
              {consoleMsg}
            </Text>
            {/* Sweep bar — only while actively scanning. */}
            <View style={styles.sweepTrack}>
              {state === 'scanning' && (
                <Animated.View
                  style={[
                    styles.sweepBar,
                    {
                      left: sweep.interpolate({
                        inputRange:  [0, 1],
                        outputRange: ['-25%', '100%'],
                      }),
                    },
                  ]}
                />
              )}
            </View>
          </View>
        )}

        {/* ── Primary action: SCAN ─────────────────────────── */}
        <TouchableOpacity
          style={[styles.scanBtn, busy && styles.btnDisabled]}
          onPress={handleScan}
          disabled={busy}
          activeOpacity={0.7}
        >
          {state === 'scanning'
            ? <ActivityIndicator color="#000" />
            : <Text style={styles.scanBtnText}>SCAN</Text>
          }
        </TouchableOpacity>

        {/* ── Flight logs (no connection required) ─────────── */}
        <TouchableOpacity
          style={styles.flightLogsBtn}
          onPress={() => navigation.navigate('Flights')}
          activeOpacity={0.7}
        >
          <Text style={styles.flightLogsBtnText}>FLIGHT LOGS →</Text>
        </TouchableOpacity>

        {/* ── Advanced disclosure ──────────────────────────── */}
        <TouchableOpacity
          style={styles.advancedToggle}
          onPress={() => setAdvancedOpen((v) => !v)}
          activeOpacity={0.6}
        >
          <Text style={styles.advancedToggleText}>
            {advancedOpen ? '▾ ADVANCED' : '▸ ADVANCED'}
          </Text>
        </TouchableOpacity>

        {advancedOpen && (
          <View style={styles.advancedBlock}>
            <Text style={styles.fieldLabel}>BOAT IP</Text>
            <TextInput
              style={styles.input}
              value={ip}
              onChangeText={setIP}
              keyboardType="numbers-and-punctuation"
              returnKeyType="done"
              onSubmitEditing={Keyboard.dismiss}
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

            <TouchableOpacity
              style={[styles.connectBtn, busy && styles.btnDisabled]}
              onPress={() => doConnect(ip.trim())}
              disabled={busy}
              activeOpacity={0.7}
            >
              {state === 'connecting'
                ? <ActivityIndicator color={Colors.accent} />
                : <Text style={styles.connectBtnText}>CONNECT</Text>
              }
            </TouchableOpacity>
          </View>
        )}
      </Pressable>
    </Screen>
  );
}

const styles = StyleSheet.create({
  inner: { flex: 1, justifyContent: 'center', paddingHorizontal: 28 },

  brandBlock: { alignItems: 'center', marginBottom: 36 },
  brand:      { color: Colors.textPrimary, fontSize: 26, fontWeight: '800', letterSpacing: 6, fontFamily: 'monospace' },
  stripe:     { flexDirection: 'row', width: 180, height: 4, marginTop: 8, borderRadius: 1, overflow: 'hidden' },
  stripeSeg:  { flex: 1, height: 4 },
  subtitle:   { color: Colors.textSecondary, fontSize: 10, letterSpacing: 3, fontFamily: 'monospace', marginTop: 12 },

  // Console block (busy / failed only)
  consoleBlock:  { backgroundColor: Colors.surface, paddingHorizontal: 12, paddingVertical: 10, borderRadius: 4, borderLeftWidth: 2, borderLeftColor: Colors.accent, marginBottom: 20 },
  consoleLine:   { fontSize: 12, fontFamily: 'monospace', lineHeight: 16 },
  consolePrompt: { color: Colors.accent, fontWeight: '800' },
  sweepTrack:    { height: 2, marginTop: 8, backgroundColor: Colors.surfaceLight, overflow: 'hidden', position: 'relative' },
  sweepBar:      { position: 'absolute', top: 0, width: '25%', height: 2, backgroundColor: Colors.accent },

  // Primary action (SCAN)
  scanBtn:      { backgroundColor: Colors.accent, paddingVertical: 18, borderRadius: 4, alignItems: 'center' },
  scanBtnText:  { color: '#000', fontWeight: '800', fontSize: 15, letterSpacing: 6, fontFamily: 'monospace' },

  // Flight logs (works offline)
  flightLogsBtn:     { marginTop: 12, paddingVertical: 12, borderRadius: 4, alignItems: 'center', backgroundColor: Colors.surface, borderWidth: 1, borderColor: Colors.surfaceLight },
  flightLogsBtnText: { color: Colors.accent, fontWeight: '800', fontSize: 12, letterSpacing: 3, fontFamily: 'monospace' },

  // Advanced disclosure
  advancedToggle:    { alignSelf: 'center', marginTop: 18, paddingVertical: 6, paddingHorizontal: 10 },
  advancedToggleText:{ color: Colors.textSecondary, fontSize: 11, letterSpacing: 3, fontFamily: 'monospace', fontWeight: '700' },

  advancedBlock: { marginTop: 12 },
  fieldLabel:    { color: Colors.textSecondary, fontSize: 10, letterSpacing: 2, fontFamily: 'monospace', marginBottom: 6 },
  input:         { backgroundColor: Colors.surface, color: Colors.textPrimary, padding: 14, borderRadius: 4, fontSize: 16, fontFamily: 'monospace', letterSpacing: 1 },
  lastIpHint:    { color: Colors.accent, fontSize: 11, fontFamily: 'monospace', marginTop: 8, letterSpacing: 1 },

  connectBtn:     { marginTop: 16, paddingVertical: 14, borderRadius: 4, alignItems: 'center', backgroundColor: Colors.surface, borderWidth: 1, borderColor: Colors.accent },
  connectBtnText: { color: Colors.accent, fontWeight: '800', fontSize: 13, letterSpacing: 3, fontFamily: 'monospace' },

  btnDisabled: { opacity: 0.4 },
});
