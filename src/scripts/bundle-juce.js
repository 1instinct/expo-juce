import fetch from 'node-fetch';
import AdmZip from 'adm-zip';
import fs from 'fs';
import path from 'path';
import { fileURLToPath } from 'url';

async function downloadJUCE() {
  const __filename = fileURLToPath(import.meta.url);
  const __dirname = path.dirname(__filename);

  // check if JUCE is already downloaded
  const juceDir = path.join(__dirname, '..', '..', 'JUCE');

  if (fs.existsSync(juceDir)) {
    console.log('JUCE already downloaded');
    return;
  }

  const juceUrl = 'https://github.com/juce-framework/JUCE/archive/refs/tags/8.0.1.zip';
  const response = await fetch(juceUrl);
  const buffer = await response.buffer();
  const zip = new AdmZip(buffer);
  zip.extractAllTo(juceDir, true);
  console.log('JUCE extracted to', juceDir);
}

downloadJUCE().catch(console.error);
