#!/usr/bin/env node
/**
 * render_diag_compare.js — Compare rendering diagnostics between MDropDX12 and Milkwave
 *
 * Connects to both apps' Named Pipes and queries GET_RENDER_DIAG,
 * then shows a side-by-side comparison highlighting differences.
 *
 * Usage: node tools/render_diag_compare.js
 */

const net = require('net');
const fs = require('fs');
const { execSync } = require('child_process');

// Find all Milkwave_* pipes and identify which belongs to MDropDX12 vs Milkwave
function findPipes() {
  const pipes = fs.readdirSync('//./pipe/').filter(p => p.startsWith('Milkwave_'));
  if (pipes.length === 0) {
    console.error('No Milkwave_* pipes found. Are both apps running?');
    process.exit(1);
  }

  const pipeInfo = [];
  for (const p of pipes) {
    const pid = p.replace('Milkwave_', '');
    let name = '';
    try {
      const out = execSync(`tasklist /FI "PID eq ${pid}" /FO CSV /NH`, { encoding: 'utf8' });
      const match = out.match(/"([^"]+)"/);
      if (match) name = match[1];
    } catch (e) {}
    pipeInfo.push({ pipe: `\\\\.\\pipe\\${p}`, pid, name });
  }

  let mdrop = null, milkwave = null;
  for (const info of pipeInfo) {
    const lower = info.name.toLowerCase();
    if (lower.includes('mdrop')) mdrop = info;
    else if (lower.includes('milkwave')) milkwave = info;
  }

  // Fallback: if we can't distinguish, use first two
  if (!mdrop && !milkwave && pipeInfo.length >= 2) {
    mdrop = pipeInfo[0];
    milkwave = pipeInfo[1];
  } else if (!mdrop && pipeInfo.length === 1) {
    mdrop = pipeInfo[0];
  } else if (!milkwave && pipeInfo.length === 1) {
    milkwave = pipeInfo[0];
  }

  return { mdrop, milkwave };
}

function queryPipe(pipePath, command, timeoutMs = 3000) {
  return new Promise((resolve, reject) => {
    const client = net.connect(pipePath, () => {
      // Send command as UTF-16LE (Windows named pipe convention)
      const cmdBuf = Buffer.from(command + '\0', 'utf16le');
      client.write(cmdBuf);
    });

    let data = Buffer.alloc(0);
    const timer = setTimeout(() => {
      client.destroy();
      reject(new Error(`Timeout reading from ${pipePath}`));
    }, timeoutMs);

    client.on('data', (chunk) => {
      data = Buffer.concat([data, chunk]);
      // Check if we have a complete RENDER_DIAG response
      const str = data.toString('utf16le').replace(/\0/g, '');
      if (str.includes('RENDER_DIAG')) {
        clearTimeout(timer);
        client.destroy();
        resolve(str);
      }
    });

    client.on('error', (err) => {
      clearTimeout(timer);
      reject(err);
    });

    client.on('end', () => {
      clearTimeout(timer);
      const str = data.toString('utf16le').replace(/\0/g, '');
      resolve(str);
    });
  });
}

function parseResponse(str) {
  // Find RENDER_DIAG message (may have other messages mixed in)
  const lines = str.split(/[\r\n]+/);
  for (const line of lines) {
    if (line.startsWith('RENDER_DIAG|')) {
      const result = {};
      const parts = line.split('|').slice(1); // skip "RENDER_DIAG"
      for (const part of parts) {
        const eq = part.indexOf('=');
        if (eq > 0) {
          const key = part.substring(0, eq);
          const val = part.substring(eq + 1);
          result[key] = val;
        }
      }
      return result;
    }
  }
  return null;
}

function colorDiff(pct) {
  if (pct < 1) return '\x1b[32m';   // green: <1%
  if (pct < 5) return '\x1b[33m';   // yellow: 1-5%
  if (pct < 20) return '\x1b[31m';  // red: 5-20%
  return '\x1b[35m';                 // magenta: >20%
}

function compareDiags(mdropData, milkwaveData) {
  const allKeys = new Set([...Object.keys(mdropData), ...Object.keys(milkwaveData)]);

  // Group keys for display
  const groups = {
    'Blur': ['blur_min0', 'blur_max0', 'blur_min1', 'blur_max1', 'blur_min2', 'blur_max2',
             'fscale0', 'fbias0', 'fscale1', 'fbias1', 'fscale2', 'fbias2', 'nHighestBlur'],
    'Warp Params': ['decay', 'gamma', 'echo_alpha', 'echo_zoom',
                    'zoom', 'rot', 'warp', 'cx', 'cy', 'dx', 'dy', 'sx', 'sy', 'zoomexp'],
    'Q Variables (1-16)': ['q1', 'q2', 'q3', 'q4', 'q5', 'q6', 'q7', 'q8',
                    'q9', 'q10', 'q11', 'q12', 'q13', 'q14', 'q15', 'q16'],
    'Q Variables (17-32)': ['q17', 'q18', 'q19', 'q20', 'q21', 'q22', 'q23', 'q24',
                    'q25', 'q26', 'q27', 'q28', 'q29', 'q30', 'q31', 'q32'],
    'Audio': ['bass_rel', 'mid_rel', 'treb_rel', 'bass_imm', 'mid_imm', 'treb_imm',
              'bass_avg', 'mid_avg', 'treb_avg', 'bass_lavg', 'mid_lavg', 'treb_lavg'],
    'Config': ['warpPSVer', 'compPSVer', 'texW', 'texH', 'aspect_x', 'aspect_y',
               'gridW', 'gridH', 'srate'],
    'Mesh UVs': ['mesh_ctr', 'mesh_tl', 'mesh_br'],
    'Runtime': ['time', 'frame', 'shapes', 'waves', 'fps', 'wave_peak'],
  };

  const RESET = '\x1b[0m';
  const BOLD = '\x1b[1m';
  const DIM = '\x1b[2m';

  console.log(`\n${BOLD}═══════════════════════════════════════════════════════════════════════════${RESET}`);
  console.log(`${BOLD}  RENDER DIAGNOSTIC COMPARISON: MDropDX12 vs Milkwave Visualizer${RESET}`);
  console.log(`${BOLD}═══════════════════════════════════════════════════════════════════════════${RESET}\n`);

  let significantDiffs = [];

  for (const [groupName, keys] of Object.entries(groups)) {
    console.log(`${BOLD}-- ${groupName} --${RESET}`);
    console.log(`${'Key'.padEnd(16)} ${'MDropDX12'.padStart(16)} ${'Milkwave'.padStart(16)}  ${'Diff'.padStart(10)}`);
    console.log(`${'---'.padEnd(16, '-')} ${'---'.padEnd(16, '-')} ${'---'.padEnd(16, '-')}  ${'---'.padEnd(10, '-')}`);

    for (const key of keys) {
      const mv = mdropData[key] || '--';
      const mw = milkwaveData[key] || '--';

      let diffStr = '';
      let diffColor = DIM;

      // Try numeric comparison
      const mvNum = parseFloat(mv);
      const mwNum = parseFloat(mw);
      if (!isNaN(mvNum) && !isNaN(mwNum)) {
        const maxAbs = Math.max(Math.abs(mvNum), Math.abs(mwNum), 0.0001);
        const pctDiff = Math.abs(mvNum - mwNum) / maxAbs * 100;
        if (pctDiff > 0.01) {
          diffStr = `${pctDiff.toFixed(1)}%`;
          diffColor = colorDiff(pctDiff);
          if (pctDiff > 5 && !['time', 'frame', 'fps'].includes(key)) {
            significantDiffs.push({ key, mv, mw, pctDiff });
          }
        } else {
          diffStr = 'match';
          diffColor = '\x1b[32m';
        }
      } else if (mv !== mw) {
        diffStr = 'DIFFER';
        diffColor = '\x1b[31m';
        if (!['time', 'frame'].includes(key)) {
          significantDiffs.push({ key, mv, mw, pctDiff: 100 });
        }
      } else {
        diffStr = 'match';
        diffColor = '\x1b[32m';
      }

      console.log(`${key.padEnd(16)} ${mv.padStart(16)} ${mw.padStart(16)}  ${diffColor}${diffStr.padStart(10)}${RESET}`);
    }
    console.log('');
  }

  if (significantDiffs.length > 0) {
    console.log(`${BOLD}\x1b[31mSIGNIFICANT DIFFERENCES (>5%):${RESET}`);
    for (const d of significantDiffs.sort((a, b) => b.pctDiff - a.pctDiff)) {
      console.log(`  ${d.key}: MDropDX12=${d.mv}, Milkwave=${d.mw} (${d.pctDiff.toFixed(1)}%)`);
    }
  } else {
    console.log(`${BOLD}\x1b[32mNo significant differences found (all values within 5%)${RESET}`);
  }
  console.log('');
}

async function main() {
  const { mdrop, milkwave } = findPipes();

  if (!mdrop || !milkwave) {
    console.log('Found pipes:');
    if (mdrop) console.log(`  MDropDX12: ${mdrop.pipe} (${mdrop.name}, PID ${mdrop.pid})`);
    if (milkwave) console.log(`  Milkwave:  ${milkwave.pipe} (${milkwave.name}, PID ${milkwave.pid})`);
    console.error('\nNeed both MDropDX12 and Milkwave Visualizer running.');
    process.exit(1);
  }

  console.log(`MDropDX12: ${mdrop.pipe} (${mdrop.name}, PID ${mdrop.pid})`);
  console.log(`Milkwave:  ${milkwave.pipe} (${milkwave.name}, PID ${milkwave.pid})`);

  try {
    // Query both apps simultaneously
    const [mdropResp, milkwaveResp] = await Promise.all([
      queryPipe(mdrop.pipe, 'GET_RENDER_DIAG'),
      queryPipe(milkwave.pipe, 'GET_RENDER_DIAG'),
    ]);

    const mdropData = parseResponse(mdropResp);
    const milkwaveData = parseResponse(milkwaveResp);

    if (!mdropData) {
      console.error('Failed to parse MDropDX12 RENDER_DIAG response');
      console.error('Raw:', mdropResp.substring(0, 200));
      process.exit(1);
    }
    if (!milkwaveData) {
      console.error('Failed to parse Milkwave RENDER_DIAG response');
      console.error('Raw:', milkwaveResp.substring(0, 200));
      process.exit(1);
    }

    compareDiags(mdropData, milkwaveData);
  } catch (err) {
    console.error('Error:', err.message);
    process.exit(1);
  }
}

main();
