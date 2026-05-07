// Screen.tsx — safe-area aware wrapper for every screen.
// Replaces raw <View style={styles.container}> on all screens.

import React from 'react';
import { StyleSheet, ViewStyle } from 'react-native';
import { SafeAreaView } from 'react-native-safe-area-context';
import { Colors } from '../constants';

interface Props {
  children: React.ReactNode;
  style?: ViewStyle;
}

export default function Screen({ children, style }: Props) {
  return (
    <SafeAreaView style={[styles.root, style]}>
      {children}
    </SafeAreaView>
  );
}

const styles = StyleSheet.create({
  root: { flex: 1, backgroundColor: Colors.background },
});
