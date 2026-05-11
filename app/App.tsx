import { StatusBar } from 'expo-status-bar';
import { NavigationContainer } from '@react-navigation/native';
import { createNativeStackNavigator } from '@react-navigation/native-stack';
import { SafeAreaProvider } from 'react-native-safe-area-context';

import ConnectionScreen from './src/screens/ConnectionScreen';
import HelmScreen from './src/screens/HelmScreen';
import MapScreen from './src/screens/MapScreen';
import TelemetryScreen from './src/screens/TelemetryScreen';
import SystemsScreen from './src/screens/SystemsScreen';

export type RootStackParamList = {
  Connection: undefined;
  Helm:       { ip: string };
  Map:        { ip: string };
  Telemetry:  { ip: string };
  Systems:    { ip: string };
};

const Stack = createNativeStackNavigator<RootStackParamList>();

export default function App() {
  return (
    <SafeAreaProvider>
      <NavigationContainer>
        <StatusBar style="light" />
        <Stack.Navigator
          initialRouteName="Connection"
          screenOptions={{
            headerShown: false,
            animation: 'slide_from_right',
            contentStyle: { backgroundColor: '#0a0f1a' },
          }}
        >
          <Stack.Screen name="Connection" component={ConnectionScreen} />
          <Stack.Screen name="Helm"       component={HelmScreen} />
          <Stack.Screen name="Map"        component={MapScreen} />
          <Stack.Screen name="Telemetry"  component={TelemetryScreen} />
          <Stack.Screen name="Systems"    component={SystemsScreen} />
        </Stack.Navigator>
      </NavigationContainer>
    </SafeAreaProvider>
  );
}
