// FPVScreen.tsx
// Full-screen MJPEG stream from ESP32-CAM with HUD overlay (heading, speed, depth, battery).
// Stream URL: http://<cameraIP>/stream

import React from 'react';
import { View, Text, StyleSheet, Dimensions } from 'react-native';
import { WebView } from 'react-native-webview';
import { NativeStackScreenProps } from '@react-navigation/native-stack';
import { RootStackParamList } from '../../App';
import { Colors, CAMERA_STREAM_PATH } from '../constants';
import { useTelemetry } from '../hooks/useTelemetry';
import EmergencyStop from '../components/EmergencyStop';

type Props = NativeStackScreenProps<RootStackParamList, 'FPV'>;
const { width, height } = Dimensions.get('window');

export default function FPVScreen({ route }: Props) {
  const { cameraIP } = route.params;
  const { data } = useTelemetry();
  const streamURL = `http://${cameraIP}${CAMERA_STREAM_PATH}`;

  return (
    <View style={styles.container}>
      <WebView
        source={{ uri: streamURL }}
        style={styles.stream}
        scalesPageToFit
        mediaPlaybackRequiresUserAction={false}
      />

      {/* HUD overlay */}
      <View style={styles.hud} pointerEvents="none">
        <Text style={styles.hudText}>HDG {data?.heading ?? '--'}°</Text>
        <Text style={styles.hudText}>{data?.speed_kts ?? '--'} kts</Text>
        <Text style={styles.hudText}>{data?.sonar_ok ? `${data.depth_m} m` : '-- m'}</Text>
        <Text style={[styles.hudText, data?.batt_low && { color: Colors.warning }]}>
          {data?.batt_v ?? '--'} V
        </Text>
      </View>

      <EmergencyStop ip={cameraIP} />
    </View>
  );
}

const styles = StyleSheet.create({
  container: { flex: 1, backgroundColor: '#000' },
  stream:    { width, height },
  hud:       { position: 'absolute', top: 40, left: 16, flexDirection: 'row', gap: 16 },
  hudText:   { color: Colors.accent, fontWeight: 'bold', fontSize: 13, textShadowColor: '#000', textShadowRadius: 4 },
});
