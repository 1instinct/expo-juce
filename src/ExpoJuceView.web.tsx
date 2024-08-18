import * as React from 'react';

import { ExpoJuceViewProps } from './ExpoJuce.types';

export default function ExpoJuceView(props: ExpoJuceViewProps) {
  return (
    <div>
      <span>{props.name}</span>
    </div>
  );
}
