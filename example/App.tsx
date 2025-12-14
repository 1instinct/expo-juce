import { StyleSheet, Text, View, Button, Alert } from 'react-native';
import { useState } from 'react';

import * as ExpoJuce from 'expo-juce';

export default function App() {
  const [lastTone, setLastTone] = useState<string>('');

  const handlePlayTone = () => {
    try {
      const frequency = 440; // A4 note
      const duration = 0.5; // 0.5 seconds
      const result = ExpoJuce.playTone(frequency, duration);
      setLastTone(result);
      Alert.alert('Tone Played', result);
    } catch (error) {
      Alert.alert('Error', `Failed to play tone: ${error}`);
    }
  };

  return (
    <View style={styles.container}>
      <Text style={styles.title}>ExpoJuce Test App</Text>
      <Text style={styles.text}>Hello: {ExpoJuce.hello()}</Text>
      <Text style={styles.text}>API Key: {ExpoJuce.getApiKey()}</Text>
      <Text style={styles.text}>PI: {ExpoJuce.PI}</Text>
      
      <View style={styles.buttonContainer}>
        <Button 
          title="Play 440Hz Tone" 
          onPress={handlePlayTone}
        />
      </View>
      
      {lastTone ? (
        <Text style={styles.result}>{lastTone}</Text>
      ) : null}
    </View>
  );
}

const styles = StyleSheet.create({
  container: {
    flex: 1,
    backgroundColor: '#fff',
    alignItems: 'center',
    justifyContent: 'center',
    padding: 20,
  },
  title: {
    fontSize: 24,
    fontWeight: 'bold',
    marginBottom: 20,
  },
  text: {
    fontSize: 16,
    marginVertical: 5,
  },
  buttonContainer: {
    marginTop: 30,
    marginBottom: 20,
  },
  result: {
    fontSize: 14,
    color: '#666',
    fontStyle: 'italic',
  },
});
