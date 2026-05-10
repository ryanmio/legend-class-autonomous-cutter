// storageService.ts
// Persistent local storage. Currently only the last connected IP — mission
// libraries and depth logs were removed with their consumer screens and
// will be reintroduced when there's a real feature to support.

import AsyncStorage from '@react-native-async-storage/async-storage';

const KEYS = {
  lastIP: 'lastIP',
} as const;

export async function saveLastIP(ip: string) {
  await AsyncStorage.setItem(KEYS.lastIP, ip);
}

export async function loadLastIP(): Promise<string | null> {
  return AsyncStorage.getItem(KEYS.lastIP);
}
