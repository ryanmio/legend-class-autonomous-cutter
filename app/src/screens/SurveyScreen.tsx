import React from 'react';
import { View, Text, StyleSheet } from 'react-native';
import { NativeStackScreenProps } from '@react-navigation/native-stack';
import { RootStackParamList } from '../../App';
import { Colors } from '../constants';
import Screen from '../components/Screen';

type Props = NativeStackScreenProps<RootStackParamList, 'Survey'>;

// PLACEHOLDER SCREEN.
// Bounding-box editor, grid generator, and bathymetric/depth logging will
// land here once we have a survey-capable boat and a working sonar feed.
// Held as a placeholder so the slot in the nav stays reserved.
export default function SurveyScreen(_: Props) {
  return (
    <Screen>
      <View style={styles.inner}>
        <Text style={styles.title}>SURVEY PLANNER</Text>
        <Text style={styles.msg}>Survey tooling lands here once sonar is wired.</Text>
      </View>
    </Screen>
  );
}

const styles = StyleSheet.create({
  inner: { flex: 1, justifyContent: 'center', alignItems: 'center', padding: 16 },
  title: { color: Colors.accent, fontSize: 18, fontWeight: 'bold', marginBottom: 12, letterSpacing: 2 },
  msg:   { color: Colors.textSecondary, fontStyle: 'italic', textAlign: 'center' },
});
