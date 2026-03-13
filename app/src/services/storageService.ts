// storageService.ts
// Persistent local storage for waypoint missions, bathymetric track logs,
// last connected IP, and app preferences.

import AsyncStorage from '@react-native-async-storage/async-storage';
import { Waypoint } from '../types';

const KEYS = {
  lastIP:       'lastIP',
  lastCamIP:    'lastCamIP',
  missions:     'missions',
  depthLog:     'depthLog',
} as const;

// ---- Connection ----
export async function saveLastIP(ip: string, camIP: string) {
  await AsyncStorage.multiSet([[KEYS.lastIP, ip], [KEYS.lastCamIP, camIP]]);
}

export async function loadLastIP(): Promise<{ ip: string; camIP: string } | null> {
  const pairs = await AsyncStorage.multiGet([KEYS.lastIP, KEYS.lastCamIP]);
  const ip    = pairs[0][1];
  const camIP = pairs[1][1];
  if (!ip) return null;
  return { ip, camIP: camIP ?? ip };
}

// ---- Waypoint missions ----
export interface Mission {
  name: string;
  waypoints: Waypoint[];
  createdAt: number;
}

export async function saveMission(mission: Mission) {
  const raw  = await AsyncStorage.getItem(KEYS.missions);
  const list: Mission[] = raw ? JSON.parse(raw) : [];
  list.push(mission);
  await AsyncStorage.setItem(KEYS.missions, JSON.stringify(list));
}

export async function loadMissions(): Promise<Mission[]> {
  const raw = await AsyncStorage.getItem(KEYS.missions);
  return raw ? JSON.parse(raw) : [];
}

export async function deleteMission(name: string) {
  const missions = await loadMissions();
  await AsyncStorage.setItem(
    KEYS.missions,
    JSON.stringify(missions.filter((m) => m.name !== name))
  );
}

// ---- Depth / bathymetric log ----
export interface DepthPoint {
  lat: number;
  lon: number;
  depthM: number;
  ts: number;
}

export async function appendDepthPoints(points: DepthPoint[]) {
  const raw      = await AsyncStorage.getItem(KEYS.depthLog);
  const existing: DepthPoint[] = raw ? JSON.parse(raw) : [];
  await AsyncStorage.setItem(KEYS.depthLog, JSON.stringify([...existing, ...points]));
}

export async function loadDepthLog(): Promise<DepthPoint[]> {
  const raw = await AsyncStorage.getItem(KEYS.depthLog);
  return raw ? JSON.parse(raw) : [];
}

export async function clearDepthLog() {
  await AsyncStorage.removeItem(KEYS.depthLog);
}

// ---- CSV export ----
export function depthLogToCSV(points: DepthPoint[]): string {
  const header = 'timestamp,lat,lon,depth_m\n';
  const rows   = points.map((p) =>
    `${new Date(p.ts).toISOString()},${p.lat},${p.lon},${p.depthM}`
  );
  return header + rows.join('\n');
}
