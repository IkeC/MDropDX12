#!/usr/bin/env node
// Quick script to send a pipe command to Milkwave Visualizer
const net = require('net');
const fs = require('fs');
const { execSync } = require('child_process');

const command = process.argv.slice(2).join(' ');
if (!command) { console.error('Usage: node send_to_milkwave.js <command>'); process.exit(1); }

const pipes = fs.readdirSync('//./pipe/').filter(p => p.startsWith('Milkwave_'));
let milkwavePipe;
for (const p of pipes) {
  const pid = p.replace('Milkwave_', '');
  try {
    const out = execSync(`tasklist /FI "PID eq ${pid}" /FO CSV /NH`, { encoding: 'utf8' });
    if (out.toLowerCase().includes('milkwavevisualizer')) {
      milkwavePipe = `\\\\.\\pipe\\${p}`;
      console.log(`Milkwave pipe: ${milkwavePipe} (PID ${pid})`);
    }
  } catch(e) {}
}
if (!milkwavePipe) { console.error('No Milkwave pipe found'); process.exit(1); }

const client = net.connect(milkwavePipe, () => {
  const cmdBuf = Buffer.from(command + '\0', 'utf16le');
  client.write(cmdBuf);
  console.log(`Sent: ${command}`);
});

let data = Buffer.alloc(0);
client.on('data', (chunk) => {
  data = Buffer.concat([data, chunk]);
});

setTimeout(() => {
  if (data.length > 0) {
    console.log('Response:', data.toString('utf16le').replace(/\0/g, ''));
  }
  client.destroy();
  process.exit(0);
}, 2000);

client.on('error', (e) => { console.error('Error:', e.message); process.exit(1); });
