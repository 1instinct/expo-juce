import { requireNativeViewManager } from 'expo-modules-core';
import * as React from 'react';

import { JucePadViewProps, PadNoteOffEvent, PadNoteOnEvent } from './ExpoJuce.types';

type NativeJucePadViewProps = Omit<JucePadViewProps, 'onNoteOn' | 'onNoteOff'> & {
  onNoteOn?: (event: { nativeEvent: PadNoteOnEvent }) => void;
  onNoteOff?: (event: { nativeEvent: PadNoteOffEvent }) => void;
};

// Resolved on first render, not at import, so importing the package never touches the native
// view registry (e.g. on a binary without the view, check isJucePadViewAvailable() first).
let NativeJucePadView: React.ComponentType<NativeJucePadViewProps> | null = null;

function getNativeView(): React.ComponentType<NativeJucePadViewProps> {
  if (NativeJucePadView === null) {
    NativeJucePadView = requireNativeViewManager('ExpoJuce');
  }
  return NativeJucePadView;
}

/**
 * Native touch pad: plays `note` from the UIKit touch handler (no JS thread in the sound path).
 * The note starts on the first touch and stops when the last touch on the view lifts.
 * `onNoteOn` / `onNoteOff` fire after the engine was called, for visuals and haptics.
 */
export default function JucePadView(props: JucePadViewProps) {
  const { onNoteOn, onNoteOff, ...rest } = props;
  const NativeView = getNativeView();
  const handleNoteOn = React.useCallback(
    (event: { nativeEvent: PadNoteOnEvent }) => onNoteOn?.(event.nativeEvent),
    [onNoteOn]
  );
  const handleNoteOff = React.useCallback(
    (event: { nativeEvent: PadNoteOffEvent }) => onNoteOff?.(event.nativeEvent),
    [onNoteOff]
  );
  return (
    <NativeView
      {...rest}
      onNoteOn={onNoteOn ? handleNoteOn : undefined}
      onNoteOff={onNoteOff ? handleNoteOff : undefined}
    />
  );
}
