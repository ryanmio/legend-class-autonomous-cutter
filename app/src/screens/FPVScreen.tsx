import React from 'react';
import { View, Text, StyleSheet } from 'react-native';
import { NativeStackScreenProps } from '@react-navigation/native-stack';
import { RootStackParamList } from '../../App';
import { Colors } from '../constants';
import Screen from '../components/Screen';

type Props = NativeStackScreenProps<RootStackParamList, 'FPV'>;

export default function FPVScreen({ route }: Props) {
  return (
    <Screen>
      <View style={styles.inner}>
        <Text style={styles.title}>FPV</Text>
        <Text style={styles.msg}>No camera module on this build.</Text>
      </View>
    </Screen>
  );
}

const styles = StyleSheet.create({
  inner: { flex: 1, justifyContent: 'center', alignItems: 'center' },
  title: { color: Colors.accent, fontSize: 18, fontWeight: 'bold', marginBottom: 12 },
  msg:   { color: Colors.textSecondary, fontStyle: 'italic' },
});
