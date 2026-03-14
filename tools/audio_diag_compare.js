// audio_diag_compare.js — Compare audio diagnostics between MDropDX12 and Milkwave
// Usage: node audio_diag_compare.js [count]
// Sends GET_AUDIO_DIAG to both visualizer pipes and displays results side by side.

const net = require('net');
const fs = require('fs');

const COUNT = parseInt(process.argv[2]) || 3;
const DELAY_MS = 1000;

function findPipes() {
  const pipes = fs.readdirSync('//./pipe/').filter(p => p.startsWith('Milkwave_'));
  const result = { mdrop: null, milkwave: null };

  for (const pipe of pipes) {
    const pid = pipe.replace('Milkwave_', '');
    // Check if PID belongs to MDropDX12 or MilkwaveVisualizer
    try {
      const { execSync } = require('child_process');
      const tasklist = execSync(`tasklist /FI "PID eq ${pid}" /FO CSV /NH`, { encoding: 'utf8' });
      if (tasklist.includes('MDropDX12')) {
        result.mdrop = `//./pipe/${pipe}`;
      } else if (tasklist.includes('MilkwaveVisualizer') || tasklist.includes('Milkwave')) {
        result.milkwave = `//./pipe/${pipe}`;
      }
    } catch (e) {}
  }
  return result;
}

function queryPipe(pipePath, timeout = 3000) {
  return new Promise((resolve, reject) => {
    const client = net.connect(pipePath, () => {
      const cmd = Buffer.from('GET_AUDIO_DIAG\0', 'utf16le');
      client.write(cmd);
    });

    let data = Buffer.alloc(0);
    client.on('data', (chunk) => {
      data = Buffer.concat([data, chunk]);
      // Try to decode and check if we have a complete response
      const str = data.toString('utf16le').replace(/\0/g, '');
      if (str.includes('AUDIO_DIAG|')) {
        client.end();
        resolve(str.trim());
      }
    });

    client.on('error', (err) => reject(err));
    setTimeout(() => { client.end(); reject(new Error('timeout')); }, timeout);
  });
}

function parseResponse(resp) {
  const parts = {};
  resp.split('|').forEach(part => {
    const eq = part.indexOf('=');
    if (eq > 0) {
      parts[part.substring(0, eq)] = part.substring(eq + 1);
    }
  });
  return parts;
}

async function main() {
  const pipes = findPipes();
  console.log('Pipes found:');
  console.log('  MDropDX12:', pipes.mdrop || '(not found)');
  console.log('  Milkwave:', pipes.milkwave || '(not found)');
  console.log('');

  if (!pipes.mdrop && !pipes.milkwave) {
    console.log('No visualizer pipes found!');
    return;
  }

  for (let i = 0; i < COUNT; i++) {
    console.log(`--- Sample ${i + 1}/${COUNT} ---`);

    const results = {};
    if (pipes.mdrop) {
      try { results.mdrop = parseResponse(await queryPipe(pipes.mdrop)); }
      catch (e) { console.log('  MDropDX12: error -', e.message); }
    }
    if (pipes.milkwave) {
      try { results.milkwave = parseResponse(await queryPipe(pipes.milkwave)); }
      catch (e) { console.log('  Milkwave: error -', e.message); }
    }

    const keys = ['bass', 'mid', 'treb', 'bass_att', 'mid_att', 'treb_att',
                  'bass_imm', 'mid_imm', 'treb_imm', 'bass_avg', 'mid_avg', 'treb_avg',
                  'pcm_min', 'pcm_max'];

    console.log('  ' + 'Field'.padEnd(12) + 'MDropDX12'.padEnd(14) + 'Milkwave'.padEnd(14) + 'Ratio');
    console.log('  ' + '-'.repeat(52));
    for (const key of keys) {
      const m = results.mdrop?.[key] || '-';
      const w = results.milkwave?.[key] || '-';
      let ratio = '';
      if (m !== '-' && w !== '-') {
        const mf = parseFloat(m);
        const wf = parseFloat(w);
        if (wf !== 0) ratio = (mf / wf).toFixed(3);
      }
      console.log('  ' + key.padEnd(12) + m.padEnd(14) + w.padEnd(14) + ratio);
    }
    console.log('');

    if (i < COUNT - 1) await new Promise(r => setTimeout(r, DELAY_MS));
  }
}

main().catch(console.error);
