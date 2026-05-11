import { StyleSheet, Text, View, Pressable, ScrollView } from 'react-native';
import { useEffect, useState, useRef } from 'react';

import * as ExpoJuce from 'expo-juce';

const NOTES = [
  { name: 'C4', midi: 60 },
  { name: 'D4', midi: 62 },
  { name: 'E4', midi: 64 },
  { name: 'F4', midi: 65 },
  { name: 'G4', midi: 67 },
  { name: 'A4', midi: 69 },
  { name: 'B4', midi: 71 },
  { name: 'C5', midi: 72 },
];

const WAVEFORMS = ['sine', 'square', 'saw', 'triangle'] as const;

export default function App() {
  const [ready, setReady] = useState(false);
  const [waveform, setWaveform] = useState<typeof WAVEFORMS[number]>('sine');
  const [activeNotes, setActiveNotes] = useState<Set<number>>(new Set());
  const activeNotesRef = useRef(activeNotes);
  activeNotesRef.current = activeNotes;

  useEffect(() => {
    ExpoJuce.setup().then(() => setReady(true));
    return () => {
      ExpoJuce.shutdown();
    };
  }, []);

  const handleNoteOn = async (midi: number) => {
    await ExpoJuce.noteOn(midi, 100);
    setActiveNotes((prev) => new Set(prev).add(midi));
  };

  const handleNoteOff = async (midi: number) => {
    await ExpoJuce.noteOff(midi);
    setActiveNotes((prev) => {
      const next = new Set(prev);
      next.delete(midi);
      return next;
    });
  };

  const handleWaveform = async (wf: typeof WAVEFORMS[number]) => {
    setWaveform(wf);
    await ExpoJuce.setWaveform(wf);
  };

  const handlePanic = async () => {
    await ExpoJuce.allNotesOff();
    setActiveNotes(new Set());
  };

  if (!ready) {
    return (
      <View style={styles.container}>
        <Text style={styles.title}>Initializing audio...</Text>
      </View>
    );
  }

  return (
    <ScrollView contentContainerStyle={styles.container}>
      <Text style={styles.title}>Beeper Polysynth</Text>
      <Text style={styles.subtitle}>8-voice polyphonic synthesizer</Text>

      {/* Waveform selector */}
      <View style={styles.section}>
        <Text style={styles.label}>Waveform</Text>
        <View style={styles.row}>
          {WAVEFORMS.map((wf) => (
            <Pressable
              key={wf}
              style={[styles.chip, waveform === wf && styles.chipActive]}
              onPress={() => handleWaveform(wf)}
            >
              <Text style={[styles.chipText, waveform === wf && styles.chipTextActive]}>
                {wf}
              </Text>
            </Pressable>
          ))}
        </View>
      </View>

      {/* Keyboard */}
      <View style={styles.section}>
        <Text style={styles.label}>Keyboard (hold to play)</Text>
        <View style={styles.keyboard}>
          {NOTES.map(({ name, midi }) => (
            <Pressable
              key={midi}
              style={[styles.key, activeNotes.has(midi) && styles.keyActive]}
              onPressIn={() => handleNoteOn(midi)}
              onPressOut={() => handleNoteOff(midi)}
            >
              <Text style={[styles.keyText, activeNotes.has(midi) && styles.keyTextActive]}>
                {name}
              </Text>
            </Pressable>
          ))}
        </View>
      </View>

      {/* Panic button */}
      <Pressable style={styles.panic} onPress={handlePanic}>
        <Text style={styles.panicText}>ALL NOTES OFF</Text>
      </Pressable>
    </ScrollView>
  );
}

const styles = StyleSheet.create({
  container: {
    flexGrow: 1,
    backgroundColor: '#1a1a2e',
    alignItems: 'center',
    justifyContent: 'center',
    padding: 20,
    paddingTop: 60,
  },
  title: {
    fontSize: 28,
    fontWeight: 'bold',
    color: '#e94560',
    marginBottom: 4,
  },
  subtitle: {
    fontSize: 14,
    color: '#888',
    marginBottom: 30,
  },
  section: {
    width: '100%',
    marginBottom: 24,
  },
  label: {
    fontSize: 13,
    color: '#666',
    textTransform: 'uppercase',
    letterSpacing: 1,
    marginBottom: 8,
  },
  row: {
    flexDirection: 'row',
    gap: 8,
  },
  chip: {
    paddingHorizontal: 16,
    paddingVertical: 8,
    borderRadius: 20,
    backgroundColor: '#16213e',
    borderWidth: 1,
    borderColor: '#333',
  },
  chipActive: {
    backgroundColor: '#e94560',
    borderColor: '#e94560',
  },
  chipText: {
    color: '#888',
    fontSize: 14,
  },
  chipTextActive: {
    color: '#fff',
    fontWeight: '600',
  },
  keyboard: {
    flexDirection: 'row',
    flexWrap: 'wrap',
    gap: 6,
  },
  key: {
    width: 70,
    height: 80,
    backgroundColor: '#16213e',
    borderRadius: 8,
    borderWidth: 1,
    borderColor: '#333',
    alignItems: 'center',
    justifyContent: 'flex-end',
    paddingBottom: 10,
  },
  keyActive: {
    backgroundColor: '#0f3460',
    borderColor: '#e94560',
  },
  keyText: {
    color: '#aaa',
    fontSize: 14,
    fontWeight: '500',
  },
  keyTextActive: {
    color: '#e94560',
  },
  panic: {
    marginTop: 20,
    paddingHorizontal: 24,
    paddingVertical: 12,
    borderRadius: 8,
    backgroundColor: '#c0392b',
  },
  panicText: {
    color: '#fff',
    fontWeight: 'bold',
    fontSize: 14,
    letterSpacing: 1,
  },
});
