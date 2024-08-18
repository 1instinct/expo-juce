import { requireNativeViewManager } from 'expo-modules-core';
import * as React from 'react';

import { ExpoJuceViewProps } from './ExpoJuce.types';

const NativeView: React.ComponentType<ExpoJuceViewProps> =
  requireNativeViewManager('ExpoJuce');

export default function ExpoJuceView(props: ExpoJuceViewProps) {
  return <NativeView {...props} />;
}
