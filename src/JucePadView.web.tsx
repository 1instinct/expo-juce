import * as React from 'react';

import { JucePadViewProps } from './ExpoJuce.types';

// No native audio on web: render children only.
export default function JucePadView(props: JucePadViewProps) {
  return <>{props.children}</>;
}
