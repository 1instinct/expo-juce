import { StyleSheet, Text, View } from 'react-native';

import * as ExpoJuce from 'expo-juce';

export default function App() {
  return (
    <View style={styles.container}>
      <Text>Hello, World</Text>
      <Text>{ExpoJuce.hello()}</Text>
      <Text>API Key: {ExpoJuce.getApiKey()}</Text>
    </View>
  );
}

const styles = StyleSheet.create({
  container: {
    flex: 1,
    backgroundColor: '#fff',
    alignItems: 'center',
    justifyContent: 'center',
  },
});
