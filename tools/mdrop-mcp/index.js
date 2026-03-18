#!/usr/bin/env node
// MCP Server for MDropDX12 / Milkwave Named Pipe IPC
// Lets Claude Code interact with running visualizers.

import { McpServer } from '@modelcontextprotocol/sdk/server/mcp.js';
import { StdioServerTransport } from '@modelcontextprotocol/sdk/server/stdio.js';
import { z } from 'zod';
import net from 'net';
import fs from 'fs';
import path from 'path';
import { fileURLToPath } from 'url';
import { execSync } from 'child_process';

// ── Pipe helpers ──

function getProcessNames(pids) {
  // Returns Map<pid, exeName> for the given PIDs
  const names = new Map();
  try {
    const output = execSync('tasklist /fo csv /nh', { encoding: 'utf8', timeout: 5000 });
    for (const line of output.split(/\r?\n/)) {
      const match = line.match(/^"([^"]+)","(\d+)"/);
      if (match) {
        const pid = parseInt(match[2]);
        if (pids.has(pid)) names.set(pid, match[1]);
      }
    }
  } catch { /* best effort */ }
  return names;
}

// Classify a pipe entry by exe name into a type tag (lowercase, no extension)
function classifyPipe(p) {
  const exe = (p.exe || '').toLowerCase();
  if (exe.includes('mdropdx12')) return 'mdrop';
  if (exe.includes('milkdrop3')) return 'milkdrop3';
  if (exe.includes('milkwavevisualizer') || exe.includes('milkwave')) return 'milkwave';
  // Unknown exe — derive tag from exe name (strip .exe, lowercase)
  const base = exe.replace(/\.exe$/i, '').replace(/[^a-z0-9]/g, '_');
  return base || 'unknown';
}

// Human-readable label from exe name or type tag
function pipeLabel(typeOrExe) {
  const known = {
    'mdrop': 'MDropDX12',
    'milkwave': 'Milkwave Visualizer',
    'milkdrop3': 'MilkDrop3',
  };
  if (known[typeOrExe]) return known[typeOrExe];
  // Fall back to exe name without extension
  return typeOrExe.replace(/\.exe$/i, '') || 'Unknown';
}

// List named pipes matching a prefix. Uses fs.readdirSync with PowerShell fallback.
function listPipes(prefix) {
  const pipeDir = '//./pipe/';
  try {
    return fs.readdirSync(pipeDir).filter(name => name.startsWith(prefix));
  } catch {
    // fs.readdirSync('//./pipe/') can fail on some Windows configs — fallback to PowerShell
    try {
      const output = execSync(
        `powershell -NoProfile -Command "[IO.Directory]::GetFiles('\\\\.\\pipe\\','${prefix}*') | ForEach-Object { [IO.Path]::GetFileName($_) }"`,
        { encoding: 'utf8', timeout: 5000 }
      );
      return output.split(/\r?\n/).filter(Boolean);
    } catch { return []; }
  }
}

function discoverPipes(target = 'auto') {
  try {
    const pipeDir = '//./pipe/';
    const pipes = listPipes('Milkwave_')
      .map(name => {
        const match = name.match(/Milkwave_(\d+)/);
        return match ? { path: pipeDir + name, pid: parseInt(match[1]), exe: null, type: null } : null;
      })
      .filter(Boolean);

    // Look up process names to classify each pipe
    if (pipes.length > 0) {
      const pidSet = new Set(pipes.map(p => p.pid));
      const names = getProcessNames(pidSet);
      for (const p of pipes) {
        p.exe = names.get(p.pid) || 'unknown';
        p.type = classifyPipe(p);
      }
    }

    // Filter by target preference ('all' returns everything unfiltered)
    if (target === 'all') {
      return pipes;
    } else if (target === 'mdrop') {
      const filtered = pipes.filter(p => p.type === 'mdrop');
      if (filtered.length > 0) return filtered;
    } else if (target === 'milkwave') {
      // 'milkwave' target = any non-MDropDX12 visualizer (Milkwave, MilkDrop3, etc.)
      const filtered = pipes.filter(p => p.type !== 'mdrop');
      if (filtered.length > 0) return filtered;
    } else if (pipes.length > 1) {
      // Auto mode: prefer MDropDX12
      const mdropPipes = pipes.filter(p => p.type === 'mdrop');
      if (mdropPipes.length > 0) return mdropPipes;
    }

    return pipes;
  } catch {
    return [];
  }
}

function sendPipeMessage(pipePath, message, expectResponse = false, overallTimeoutMs = 5000) {
  return new Promise((resolve, reject) => {
    let settled = false;
    const settle = (fn, val) => { if (!settled) { settled = true; fn(val); } };

    // Overall timeout to prevent hanging
    const overallTimer = setTimeout(() => {
      try { client.destroy(); } catch {}
      settle(reject, new Error(`Pipe timeout after ${overallTimeoutMs}ms`));
    }, overallTimeoutMs);

    const client = net.connect(pipePath, () => {
      // Send as UTF-16LE with null terminator
      const sendBuf = Buffer.from(message + '\0', 'utf16le');
      client.write(sendBuf);

      if (!expectResponse) {
        // Pipe is message-mode — write completes atomically, minimal flush needed
        setTimeout(() => { client.end(); clearTimeout(overallTimer); settle(resolve, 'OK'); }, 10);
        return;
      }

      // Collect response data — resolve on null-terminator (UTF-16LE \0\0)
      let recvBuf = Buffer.alloc(0);
      let timer = null;

      const finish = () => {
        if (timer) clearTimeout(timer);
        clearTimeout(overallTimer);
        client.end();
        // Decode UTF-16LE, strip null terminators
        const text = recvBuf.toString('utf16le').replace(/\0/g, '');
        settle(resolve, text);
      };

      client.on('data', (chunk) => {
        recvBuf = Buffer.concat([recvBuf, chunk]);
        // Check for null terminator: two zero bytes at a UTF-16 boundary
        if (recvBuf.length >= 2) {
          const last2 = recvBuf.readUInt16LE(recvBuf.length - 2);
          if (last2 === 0) {
            // Complete message received — resolve immediately
            finish();
            return;
          }
        }
        // Fallback timer in case response has no null terminator (legacy)
        if (timer) clearTimeout(timer);
        timer = setTimeout(finish, 150);
      });

      // Safety timeout if no data at all
      timer = setTimeout(finish, 500);
    });

    client.on('error', (err) => {
      clearTimeout(overallTimer);
      settle(reject, new Error(`Pipe connection failed: ${err.message}`));
    });
  });
}

// Send multiple fire-and-forget messages on a single connection (avoids per-message connect overhead)
function sendPipeBatch(pipePath, messages) {
  return new Promise((resolve, reject) => {
    const client = net.connect(pipePath, () => {
      for (const msg of messages) {
        client.write(Buffer.from(msg + '\0', 'utf16le'));
      }
      setTimeout(() => { client.end(); resolve('OK'); }, 10);
    });
    client.on('error', (err) => {
      reject(new Error(`Pipe batch failed: ${err.message}`));
    });
  });
}

// ── Restart helpers ──

// Get the exe path for a given PID
function getExePath(pid) {
  try {
    const output = execSync(
      `wmic process where "ProcessId=${pid}" get ExecutablePath /value`,
      { encoding: 'utf8', timeout: 5000 }
    );
    const match = output.match(/ExecutablePath=(.+)/);
    return match ? match[1].trim() : null;
  } catch { return null; }
}

// Restart a visualizer: kill the process, relaunch its exe, wait for its pipe to appear
// Returns the new pipe entry or null on failure
async function restartVisualizer(pipe, log) {
  const exePath = getExePath(pipe.pid);
  if (!exePath) {
    log(`Could not find exe path for PID ${pipe.pid}`);
    return null;
  }

  log(`Restarting ${pipeLabel(pipe.type)} (PID ${pipe.pid})...`);

  // Kill the old process
  try { execSync(`taskkill /PID ${pipe.pid} /F`, { timeout: 5000 }); } catch { /* may already be dead */ }

  // Wait a moment for the process to fully exit
  await new Promise(r => setTimeout(r, 1000));

  // Relaunch from its own directory
  const exeDir = path.dirname(exePath);
  try {
    execSync(`start "" "${exePath}"`, { cwd: exeDir, timeout: 5000, shell: true });
  } catch (e) {
    log(`Failed to relaunch: ${e.message}`);
    return null;
  }

  // Poll for the new pipe to appear (up to 10 seconds)
  const targetType = pipe.type;
  for (let elapsed = 0; elapsed < 10000; elapsed += 500) {
    await new Promise(r => setTimeout(r, 500));
    const pipes = discoverPipes(targetType);
    if (pipes.length > 0 && pipes[0].pid !== pipe.pid) {
      log(`${pipeLabel(targetType)} restarted (new PID ${pipes[0].pid})`);
      return pipes[0];
    }
  }

  log(`${pipeLabel(targetType)} did not reconnect within timeout`);
  return null;
}

// ── Cached connection ──

let cachedPipePath = null;

function findPipe() {
  // Check if cached pipe still exists
  if (cachedPipePath) {
    const pipeName = cachedPipePath.split('/').pop();
    try {
      const exists = fs.readdirSync('//./pipe/').includes(pipeName);
      if (exists) return cachedPipePath;
    } catch { /* fall through */ }
    cachedPipePath = null;
  }

  const pipes = discoverPipes();
  if (pipes.length === 0) {
    throw new Error('No running visualizer found (MDropDX12 or Milkwave Visualizer). Start a visualizer first.');
  }

  cachedPipePath = pipes[0].path;
  return cachedPipePath;
}

async function send(message, expectResponse = false) {
  const pipePath = findPipe();
  try {
    return await sendPipeMessage(pipePath, message, expectResponse);
  } catch {
    // Pipe may have recycled — rediscover and retry once
    cachedPipePath = null;
    const retryPath = findPipe();
    return sendPipeMessage(retryPath, message, expectResponse);
  }
}

// ── Capture helpers ──

const __dirname = path.dirname(fileURLToPath(import.meta.url));
const projectRoot = path.resolve(__dirname, '../..');

function findCaptureDir() {
  if (process.env.MDROP_CAPTURE_DIR) {
    return process.env.MDROP_CAPTURE_DIR;
  }

  // Try to find capture dir from the connected process's exe location
  try {
    const pipePath = findPipe();
    const pidMatch = pipePath.match(/Milkwave_(\d+)/);
    if (pidMatch) {
      const pid = pidMatch[1];
      const output = execSync(
        `wmic process where "ProcessId=${pid}" get ExecutablePath /value`,
        { encoding: 'utf8', timeout: 5000 }
      );
      const exeMatch = output.match(/ExecutablePath=(.+)/);
      if (exeMatch) {
        const exeDir = path.dirname(exeMatch[1].trim());
        const captureDir = path.join(exeDir, 'capture');
        return captureDir; // visualizer creates it on first capture
      }
    }
  } catch { /* fall through to hardcoded paths */ }

  // Fallback: MDropDX12 build directories
  const releasePath = path.join(projectRoot, 'src/mDropDX12/Release_x64/capture');
  if (fs.existsSync(releasePath)) return releasePath;
  const debugPath = path.join(projectRoot, 'src/mDropDX12/Debug_x64/capture');
  if (fs.existsSync(debugPath)) return debugPath;
  return releasePath;
}

function waitForNewCapture(dir, afterTimestamp, timeoutMs) {
  return new Promise((resolve) => {
    const pollInterval = 100;
    let elapsed = 0;

    const check = () => {
      if (elapsed >= timeoutMs) { resolve(null); return; }
      try {
        if (fs.existsSync(dir)) {
          const files = fs.readdirSync(dir)
            .filter(f => f.endsWith('.png'))
            .map(f => {
              const fullPath = path.join(dir, f);
              const stat = fs.statSync(fullPath);
              return { path: fullPath, mtime: stat.mtimeMs };
            })
            .filter(f => f.mtime >= afterTimestamp - 1000)
            .sort((a, b) => b.mtime - a.mtime);

          if (files.length > 0 && fs.statSync(files[0].path).size > 0) {
            resolve(files[0].path);
            return;
          }
        }
      } catch { /* dir may not exist yet */ }
      elapsed += pollInterval;
      setTimeout(check, pollInterval);
    };

    setTimeout(check, 50);
  });
}

// ── Preset path resolution ──

// Extract preset directory from a STATE response string
function extractPresetDir(stateText) {
  const match = stateText.match(/PRESET=([^\r\n|]+)/);
  if (match) {
    const fullPath = match[1].trim();
    const dir = path.dirname(fullPath);
    if (dir && dir !== '.') return dir;
  }
  return null;
}

// If preset is a bare filename (no path separators), resolve to full path
// by querying STATE from the given pipe to get the current preset directory.
async function resolvePresetPath(preset, pipePath) {
  // Already a full or relative path
  if (preset.includes('\\') || preset.includes('/') || preset.includes(':')) {
    return preset;
  }
  try {
    const state = await sendPipeMessage(pipePath, 'STATE', true);
    const dir = extractPresetDir(state);
    if (dir) {
      // Check immediate directory first
      const direct = path.join(dir, preset);
      if (fs.existsSync(direct)) return direct;
      // Search subdirectories (e.g. Shader/)
      try {
        for (const entry of fs.readdirSync(dir, { withFileTypes: true })) {
          if (entry.isDirectory()) {
            const sub = path.join(dir, entry.name, preset);
            if (fs.existsSync(sub)) return sub;
          }
        }
      } catch { /* best effort */ }
      return direct; // fall back to original path
    }
  } catch { /* fall through */ }
  return preset; // best effort — send as-is
}

// ── MCP Server ──

const server = new McpServer({
  name: 'mdrop',
  version: '1.0.0',
});

// Tool: Connect / discover
server.tool(
  'mdrop_connect',
  'Discover and connect to a running visualizer. Use target to pick which one: "mdrop" (MDropDX12), "milkwave" (Milkwave Visualizer), or "auto" (prefer MDropDX12). Lists all found instances.',
  {
    target: z.enum(['auto', 'mdrop', 'milkwave']).optional()
      .describe('Which visualizer to connect to: "mdrop", "milkwave", or "auto" (default, prefers MDropDX12)'),
  },
  async ({ target }) => {
    // Always discover ALL pipes first to show what's available
    const allPipes = discoverPipes('all');
    if (allPipes.length === 0) {
      return { content: [{ type: 'text', text: 'No running visualizer found (MDropDX12 or Milkwave Visualizer). Start a visualizer first.' }] };
    }

    // Now filter by target preference
    const chosen = discoverPipes(target || 'auto');
    cachedPipePath = chosen[0].path;

    // Build a summary of all discovered instances
    const summary = allPipes.map(p => {
      const label = pipeLabel(p.type);
      const active = p.path === cachedPipePath ? ' ← connected' : '';
      return `  ${label} (PID ${p.pid})${active}`;
    }).join('\n');

    return { content: [{ type: 'text', text: `Found ${allPipes.length} visualizer(s):\n${summary}\n\nPipe: ${cachedPipePath}` }] };
  }
);

// Tool: Query state
server.tool(
  'mdrop_state',
  'Query current visualizer state (preset, opacity, quality, colors, FFT, etc.)',
  {},
  async () => {
    try {
      const response = await send('STATE', true);
      return { content: [{ type: 'text', text: response || '(no response — visualizer may not support STATE query)' }] };
    } catch (err) {
      return { content: [{ type: 'text', text: `Error: ${err.message}` }] };
    }
  }
);

// Tool: Load preset
server.tool(
  'mdrop_load_preset',
  'Load a preset by filename or full path',
  { preset: z.string().describe('Preset filename (e.g. "MyPreset.milk") or full path') },
  async ({ preset }) => {
    try {
      const pipePath = findPipe();
      const resolved = await resolvePresetPath(preset, pipePath);
      await sendPipeMessage(pipePath, `PRESET=${resolved}`);
      return { content: [{ type: 'text', text: `Loaded preset: ${resolved}` }] };
    } catch (err) {
      return { content: [{ type: 'text', text: `Error: ${err.message}` }] };
    }
  }
);

// Tool: Next preset
server.tool(
  'mdrop_next_preset',
  'Switch to the next preset',
  {},
  async () => {
    try {
      await send('SIGNAL|NEXT_PRESET');
      return { content: [{ type: 'text', text: 'Switched to next preset' }] };
    } catch (err) {
      return { content: [{ type: 'text', text: `Error: ${err.message}` }] };
    }
  }
);

// Tool: Previous preset
server.tool(
  'mdrop_prev_preset',
  'Switch to the previous preset',
  {},
  async () => {
    try {
      await send('SIGNAL|PREV_PRESET');
      return { content: [{ type: 'text', text: 'Switched to previous preset' }] };
    } catch (err) {
      return { content: [{ type: 'text', text: `Error: ${err.message}` }] };
    }
  }
);

// Tool: Send text message
server.tool(
  'mdrop_send_message',
  'Display a text message on the visualizer',
  {
    text: z.string().describe('Message text to display'),
    font: z.string().optional().describe('Font name (default: Arial)'),
    size: z.number().optional().describe('Font size (default: 32)'),
    r: z.number().optional().describe('Red 0-255 (default: 255)'),
    g: z.number().optional().describe('Green 0-255 (default: 255)'),
    b: z.number().optional().describe('Blue 0-255 (default: 255)'),
    duration: z.number().optional().describe('Display duration in seconds (default: 5)'),
  },
  async ({ text, font, size, r, g, b, duration }) => {
    try {
      let msg = `MSG|text=${text}`;
      if (font) msg += `|font=${font}`;
      msg += `|size=${size || 32}`;
      msg += `|r=${r ?? 255}|g=${g ?? 255}|b=${b ?? 255}`;
      msg += `|time=${duration || 5}`;
      await send(msg);
      return { content: [{ type: 'text', text: `Message sent: "${text}"` }] };
    } catch (err) {
      return { content: [{ type: 'text', text: `Error: ${err.message}` }] };
    }
  }
);

// Tool: Set color
server.tool(
  'mdrop_set_color',
  'Adjust hue, saturation, and/or brightness',
  {
    hue: z.number().min(0).max(1).optional().describe('Hue shift 0.0-1.0'),
    saturation: z.number().min(-1).max(1).optional().describe('Saturation -1.0 to 1.0'),
    brightness: z.number().min(-1).max(1).optional().describe('Brightness -1.0 to 1.0'),
  },
  async ({ hue, saturation, brightness }) => {
    try {
      const results = [];
      if (hue !== undefined) { await send(`COL_HUE=${hue}`); results.push(`hue=${hue}`); }
      if (saturation !== undefined) { await send(`COL_SATURATION=${saturation}`); results.push(`sat=${saturation}`); }
      if (brightness !== undefined) { await send(`COL_BRIGHTNESS=${brightness}`); results.push(`brt=${brightness}`); }
      return { content: [{ type: 'text', text: results.length ? `Set: ${results.join(', ')}` : 'No parameters specified' }] };
    } catch (err) {
      return { content: [{ type: 'text', text: `Error: ${err.message}` }] };
    }
  }
);

// ── Comparison output directory ──
const comparisonDir = path.resolve(__dirname, '../comparison');

// Helper: resize a capture image for Claude using ImageMagick
// Returns { data: base64, mimeType } for inline use, or null on failure
function resizeForClaude(filePath, maxDim = 800, quality = 85) {
  try {
    const tmpPath = filePath.replace(/\.png$/i, '_claude.jpg');
    execSync(
      `magick "${filePath}" -resize ${maxDim}x${maxDim}^> -quality ${quality} "${tmpPath}"`,
      { timeout: 10000 }
    );
    const data = fs.readFileSync(tmpPath).toString('base64');
    fs.unlinkSync(tmpPath); // clean up temp file
    return { data, mimeType: 'image/jpeg' };
  } catch {
    // Fallback: read original PNG
    try {
      const data = fs.readFileSync(filePath).toString('base64');
      return { data, mimeType: 'image/png' };
    } catch { return null; }
  }
}

// Helper: downsize a capture to the comparison directory (no base64, disk only)
// Returns the output file path, or null on failure
function downsizeTo(filePath, outName, maxDim = 800, quality = 80) {
  try {
    if (!fs.existsSync(comparisonDir)) fs.mkdirSync(comparisonDir, { recursive: true });
    const outPath = path.join(comparisonDir, outName);
    execSync(
      `magick "${filePath}" -resize ${maxDim}x${maxDim}^> -quality ${quality} "${outPath}"`,
      { timeout: 10000 }
    );
    return outPath;
  } catch { return null; }
}

// Helper: wait for a specific file to appear on disk (for deferred DX12 captures)
function waitForFile(filePath, timeoutMs = 3000) {
  return new Promise((resolve) => {
    const pollInterval = 100;
    let elapsed = 0;
    const check = () => {
      try {
        if (fs.existsSync(filePath) && fs.statSync(filePath).size > 0) {
          resolve(true);
          return;
        }
      } catch { /* not ready yet */ }
      elapsed += pollInterval;
      if (elapsed >= timeoutMs) { resolve(false); return; }
      setTimeout(check, pollInterval);
    };
    setTimeout(check, 50);
  });
}

// Helper: send a command and wait for a response matching a prefix, ignoring unrelated broadcasts.
// The pipe broadcasts to ALL clients, so a capture connection may receive stale broadcasts
// (e.g. DEVICE_VOLUME=...) before the actual CAPTURE_PATH= response arrives.
function sendAndWaitFor(pipePath, message, prefix, timeoutMs = 5000) {
  return new Promise((resolve, reject) => {
    let settled = false;
    const settle = (fn, val) => { if (!settled) { settled = true; fn(val); } };

    const overallTimer = setTimeout(() => {
      try { client.destroy(); } catch {}
      settle(reject, new Error(`Timed out waiting for ${prefix} response`));
    }, timeoutMs);

    const client = net.connect(pipePath, () => {
      const sendBuf = Buffer.from(message + '\0', 'utf16le');
      client.write(sendBuf);

      let recvBuf = Buffer.alloc(0);

      client.on('data', (chunk) => {
        recvBuf = Buffer.concat([recvBuf, chunk]);

        // Split on null terminators — each message is null-terminated UTF-16LE
        while (recvBuf.length >= 2) {
          // Find first null wchar (two zero bytes at even offset)
          let nullIdx = -1;
          for (let i = 0; i < recvBuf.length - 1; i += 2) {
            if (recvBuf[i] === 0 && recvBuf[i + 1] === 0) {
              nullIdx = i;
              break;
            }
          }
          if (nullIdx < 0) break; // no complete message yet

          // Extract one message (everything before the null)
          const msgBuf = recvBuf.subarray(0, nullIdx);
          recvBuf = recvBuf.subarray(nullIdx + 2); // skip past the null wchar

          const text = msgBuf.toString('utf16le');
          if (text.startsWith(prefix)) {
            // Found the response we're waiting for
            clearTimeout(overallTimer);
            client.end();
            settle(resolve, text);
            return;
          }
          // else: unrelated broadcast — discard and keep reading
        }
      });
    });

    client.on('error', (err) => {
      clearTimeout(overallTimer);
      settle(reject, new Error(`Pipe connection failed: ${err.message}`));
    });
  });
}

// Helper: send CAPTURE command and parse CAPTURE_PATH response
async function captureWithPath(pipePath) {
  const response = await sendAndWaitFor(pipePath, 'CAPTURE', 'CAPTURE_PATH=', 5000);
  // Parse CAPTURE_PATH=<full path> from response
  const match = response.match(/CAPTURE_PATH=(.+)/);
  if (!match || match[1] === 'ERROR') {
    return { error: response || 'Capture failed' };
  }
  const filePath = match[1].trim();
  // DX12 capture is deferred — wait for the file to appear
  const ready = await waitForFile(filePath, 3000);
  if (!ready) {
    return { error: `File not ready within timeout: ${filePath}` };
  }
  return { filePath };
}

// Tool: Capture screenshot
server.tool(
  'mdrop_capture',
  'Take a screenshot of the current visualizer output and return the image',
  {
    return_image: z.boolean().optional().describe('Return the PNG image data (default: true). Set false for just confirmation.'),
  },
  async ({ return_image }) => {
    try {
      const pipePath = findPipe();
      const result = await captureWithPath(pipePath);

      if (result.error) {
        return { content: [{ type: 'text', text: `Capture error: ${result.error}` }] };
      }

      if (return_image === false) {
        return { content: [{ type: 'text', text: `Saved: ${result.filePath}` }] };
      }

      const img = resizeForClaude(result.filePath);
      if (!img) {
        return { content: [{ type: 'text', text: `Saved but failed to read: ${result.filePath}` }] };
      }

      return {
        content: [
          { type: 'image', data: img.data, mimeType: img.mimeType },
          { type: 'text', text: `Saved: ${path.basename(result.filePath)}` },
        ]
      };
    } catch (err) {
      return { content: [{ type: 'text', text: `Error: ${err.message}` }] };
    }
  }
);

// Tool: Set FFT parameters
server.tool(
  'mdrop_set_fft',
  'Adjust FFT attack and decay smoothing',
  {
    attack: z.number().min(0).max(1).optional().describe('FFT attack 0.0-1.0'),
    decay: z.number().min(0).max(1).optional().describe('FFT decay 0.0-1.0'),
  },
  async ({ attack, decay }) => {
    try {
      const results = [];
      if (attack !== undefined) { await send(`FFT_ATTACK=${attack}`); results.push(`attack=${attack}`); }
      if (decay !== undefined) { await send(`FFT_DECAY=${decay}`); results.push(`decay=${decay}`); }
      return { content: [{ type: 'text', text: results.length ? `Set FFT: ${results.join(', ')}` : 'No parameters specified' }] };
    } catch (err) {
      return { content: [{ type: 'text', text: `Error: ${err.message}` }] };
    }
  }
);

// Tool: Get or set Windows device volume and mute
server.tool(
  'mdrop_set_volume',
  'Get or set the Windows system volume and mute state of the audio device MDropDX12 is capturing from',
  {
    volume: z.number().min(0).max(1).optional().describe('Volume level 0.0-1.0. Omit to leave unchanged.'),
    mute: z.boolean().optional().describe('Mute state. Omit to leave unchanged.'),
  },
  async ({ volume, mute }) => {
    try {
      const results = [];
      if (volume !== undefined) {
        const resp = await send(`SET_DEVICE_VOLUME=${volume}`, true);
        results.push(resp || `volume=${volume}`);
      }
      if (mute !== undefined) {
        const resp = await send(`SET_DEVICE_MUTE=${mute ? 1 : 0}`, true);
        results.push(resp || `mute=${mute}`);
      }
      if (volume === undefined && mute === undefined) {
        const resp = await send('GET_DEVICE_VOLUME', true);
        return { content: [{ type: 'text', text: resp || 'Unknown' }] };
      }
      return { content: [{ type: 'text', text: results.join(', ') }] };
    } catch (err) {
      return { content: [{ type: 'text', text: `Error: ${err.message}` }] };
    }
  }
);

// Tool: Get or set log level
server.tool(
  'mdrop_set_loglevel',
  'Get or set the visualizer log level (0=Off, 1=Error, 2=Warn, 3=Info, 4=Verbose)',
  {
    level: z.number().int().min(0).max(4).optional()
      .describe('Log level to set (0=Off, 1=Error, 2=Warn, 3=Info, 4=Verbose). Omit to query current level.'),
  },
  async ({ level }) => {
    try {
      if (level !== undefined) {
        const response = await send(`SET_LOGLEVEL=${level}`, true);
        return { content: [{ type: 'text', text: response || `Set log level: ${level}` }] };
      } else {
        const response = await send('GET_LOGLEVEL', true);
        return { content: [{ type: 'text', text: response || 'Unknown' }] };
      }
    } catch (err) {
      return { content: [{ type: 'text', text: `Error: ${err.message}` }] };
    }
  }
);

// Tool: Clear all log files
server.tool(
  'mdrop_clear_logs',
  'Delete all files in the log/ directory and re-open debug.log. Useful for clearing stale diagnostics before a fresh test.',
  {},
  async () => {
    try {
      const response = await send('CLEAR_LOGS', true);
      return { content: [{ type: 'text', text: response || 'Logs cleared' }] };
    } catch (err) {
      return { content: [{ type: 'text', text: `Error: ${err.message}` }] };
    }
  }
);

// Tool: Clean shutdown
server.tool(
  'mdrop_shutdown',
  'Cleanly shut down the visualizer (saves settings, stops render thread, exits). Useful for restarting after a build.',
  {},
  async () => {
    try {
      const response = await send('SHUTDOWN', true);
      return { content: [{ type: 'text', text: response || 'Shutdown initiated' }] };
    } catch (err) {
      // Connection reset is expected — the app is closing
      if (err.message.includes('EPIPE') || err.message.includes('ERR_STREAM') || err.message.includes('reset'))
        return { content: [{ type: 'text', text: 'Shutdown initiated (pipe closed)' }] };
      return { content: [{ type: 'text', text: `Error: ${err.message}` }] };
    }
  }
);

// Tool: Shader import (load JSON, convert GLSL→HLSL, apply)
server.tool(
  'mdrop_shader_import',
  'Import a Shadertoy shader from a .json file — loads, converts GLSL→HLSL, and applies to the visualizer. Returns conversion result. Use for debugging shader import issues.',
  {
    file_path: z.string().describe('Full path to the shader_import .json file'),
  },
  async ({ file_path }) => {
    try {
      // Normalize path separators for Windows
      const normalizedPath = file_path.replace(/\//g, '\\');
      const response = await send(`SHADER_IMPORT=${normalizedPath}`, true);
      return { content: [{ type: 'text', text: response || '(no response — check debug.log)' }] };
    } catch (err) {
      return { content: [{ type: 'text', text: `Error: ${err.message}` }] };
    }
  }
);

// Tool: Load preset list
server.tool(
  'mdrop_load_list',
  'Load a saved preset list by name, or clear the active list to revert to directory scanning',
  {
    list_name: z.string().optional().describe('Name of the preset list to load (without .txt). Omit to clear the active list.'),
  },
  async ({ list_name }) => {
    try {
      if (!list_name) {
        const response = await send('CLEAR_LIST', true);
        return { content: [{ type: 'text', text: response || 'Cleared preset list' }] };
      }
      const response = await send(`LOAD_LIST=${list_name}`, true);
      return { content: [{ type: 'text', text: response || `Loaded list: ${list_name}` }] };
    } catch (err) {
      return { content: [{ type: 'text', text: `Error: ${err.message}` }] };
    }
  }
);

// Tool: Enumerate preset lists
server.tool(
  'mdrop_enum_lists',
  'List all available saved preset lists',
  {},
  async () => {
    try {
      const response = await send('ENUM_LISTS', true);
      return { content: [{ type: 'text', text: response || '(no lists found)' }] };
    } catch (err) {
      return { content: [{ type: 'text', text: `Error: ${err.message}` }] };
    }
  }
);

// Tool: Set preset directory
server.tool(
  'mdrop_set_dir',
  'Change the preset directory and optionally enable recursive subdirectory scanning',
  {
    directory: z.string().describe('Full path to the preset directory'),
    recursive: z.boolean().optional().describe('Enable recursive subdirectory scanning (default: false)'),
  },
  async ({ directory, recursive }) => {
    try {
      let cmd = `SET_DIR=${directory}`;
      if (recursive) cmd += '|recursive';
      const response = await send(cmd, true);
      return { content: [{ type: 'text', text: response || `Set directory: ${directory}` }] };
    } catch (err) {
      return { content: [{ type: 'text', text: `Error: ${err.message}` }] };
    }
  }
);

// Helper: get window rect for a process by PID using PowerShell
function getWindowRect(pid) {
  try {
    // PowerShell one-liner to get main window position and size
    const ps = `Add-Type -Name W -Namespace Win32 -MemberDefinition '[DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r); [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L,T,R,B; }'; $p=Get-Process -Id ${pid} -EA Stop; $r=New-Object Win32.W+RECT; [Win32.W]::GetWindowRect($p.MainWindowHandle,[ref]$r)|Out-Null; "$($r.L),$($r.T),$($r.R),$($r.B)"`;
    const output = execSync(`powershell -NoProfile -Command "${ps}"`, { encoding: 'utf8', timeout: 5000 }).trim();
    const [l, t, r, b] = output.split(',').map(Number);
    if ([l, t, r, b].some(isNaN)) return null;
    return { left: l, top: t, right: r, bottom: b, width: r - l, height: b - t };
  } catch { return null; }
}

// Helper: get monitor work area for a point using PowerShell
function getMonitorWorkArea(x, y) {
  try {
    const ps = `Add-Type -AssemblyName System.Windows.Forms; $s=[System.Windows.Forms.Screen]::FromPoint([System.Drawing.Point]::new(${x},${y})); $w=$s.WorkingArea; "$($w.X),$($w.Y),$($w.Width),$($w.Height)"`;
    const output = execSync(`powershell -NoProfile -Command "${ps}"`, { encoding: 'utf8', timeout: 5000 }).trim();
    const [mx, my, mw, mh] = output.split(',').map(Number);
    if ([mx, my, mw, mh].some(isNaN)) return null;
    return { left: mx, top: my, width: mw, height: mh, right: mx + mw, bottom: my + mh };
  } catch { return null; }
}

// Helper: parse renderwin from MDropDX12 STATE response
function parseMdropWindowRect(stateText) {
  const m = stateText.match(/renderwin=\((-?\d+),(-?\d+)\)-\((-?\d+),(-?\d+)\)/);
  if (!m) return null;
  const [, l, t, r, b] = m.map(Number);
  return { left: l, top: t, right: r, bottom: b, width: r - l, height: b - t };
}

// Tool: Compare both visualizers side by side
server.tool(
  'mdrop_compare',
  'Load the same preset on both MDropDX12 and Milkwave Visualizer, position them side-by-side, then capture screenshots from both. Returns both images for visual comparison.',
  {
    preset: z.string().optional().describe('Preset filename to load on both (e.g. "MyPreset.milk"). Omit to just capture current state.'),
    delay: z.number().optional().describe('Seconds to wait after loading preset before capturing (default: 2)'),
  },
  async ({ preset, delay }) => {
    try {
      // Discover all pipes with process identification (must use 'all' to get both)
      const allPipes = discoverPipes('all');
      if (allPipes.length === 0) {
        return { content: [{ type: 'text', text: 'No running visualizers found.' }] };
      }

      if (allPipes.length < 2) {
        const found = allPipes.map(p => pipeLabel(p.type)).join(', ') || 'none';
        return { content: [{ type: 'text', text: `Need at least 2 visualizers running for comparison. Found: ${found}` }] };
      }

      // Pick the first two distinct visualizers (prefer mdrop as primary)
      const mdropPipe = allPipes.find(p => p.type === 'mdrop') || allPipes[0];
      const milkwavePipe = allPipes.find(p => p !== mdropPipe) || allPipes[1];

      const results = [];

      // --- Position MDropDX12 alongside Milkwave ---
      const primaryLabel = pipeLabel(mdropPipe.type);
      const secondaryLabel = pipeLabel(milkwavePipe.type);

      const mwRect = getWindowRect(milkwavePipe.pid);
      if (mwRect) {
        // Get primary visualizer's current state to check current size
        const mdropStateForSize = await sendPipeMessage(mdropPipe.path, 'STATE', true).catch(() => '');
        const mdRect = parseMdropWindowRect(mdropStateForSize);

        // Only reposition if sizes differ
        const alreadyMatched = mdRect &&
          mdRect.width === mwRect.width && mdRect.height === mwRect.height;

        if (!alreadyMatched) {
          // Find the monitor the secondary visualizer is on
          const mwCenterX = mwRect.left + Math.floor(mwRect.width / 2);
          const mwCenterY = mwRect.top + Math.floor(mwRect.height / 2);
          const monitor = getMonitorWorkArea(mwCenterX, mwCenterY);

          if (monitor) {
            const spaceRight = monitor.right - mwRect.right;
            const spaceLeft = mwRect.left - monitor.left;
            const spaceBelow = monitor.bottom - mwRect.bottom;
            const spaceAbove = mwRect.top - monitor.top;

            let newX, newY;
            const canFitHorizontally = Math.max(spaceRight, spaceLeft) >= mwRect.width;
            const canFitVertically = Math.max(spaceBelow, spaceAbove) >= mwRect.height;

            if (canFitHorizontally) {
              newX = spaceRight >= spaceLeft ? mwRect.right : mwRect.left - mwRect.width;
              newY = mwRect.top;
            } else if (canFitVertically) {
              newX = mwRect.left;
              newY = spaceBelow >= spaceAbove ? mwRect.bottom : mwRect.top - mwRect.height;
            } else {
              newX = spaceRight >= spaceLeft ? mwRect.right : mwRect.left - mwRect.width;
              newY = mwRect.top;
            }

            const setCmd = `SET_WINDOW=${newX},${newY},${mwRect.width},${mwRect.height}`;
            const posResult = await sendPipeMessage(mdropPipe.path, setCmd, true).catch(e => e.message);
            results.push({ type: 'text', text: `Positioned ${primaryLabel}: ${posResult}` });
          }
        } else {
          results.push({ type: 'text', text: `${primaryLabel} already matches ${secondaryLabel} size — skipping reposition` });
        }
      }

      // --- Sync audio device: query MDropDX12's active device, set it on Milkwave ---
      try {
        const devResp = await sendPipeMessage(mdropPipe.path, 'GET_AUDIO_DEVICES', true);
        const activeMatch = devResp.match(/\|active=(.+)/);
        if (activeMatch) {
          const activeDevice = activeMatch[1].trim();
          const devCmd = `DEVICE=OUT|${activeDevice}`;
          const devResult = await sendPipeMessage(milkwavePipe.path, devCmd, true).catch(e => e.message);
          results.push({ type: 'text', text: `Audio sync: ${devResult}` });
        }
      } catch { /* best effort — don't fail the compare */ }

      // Helper: verify preset is loaded by polling STATE until PRESET= contains the expected filename
      const verifyPresetLoaded = async (pipePath, presetBasename, timeoutMs = 8000) => {
        const target = presetBasename.toLowerCase();
        for (let elapsed = 0; elapsed < timeoutMs; elapsed += 500) {
          try {
            const state = await sendPipeMessage(pipePath, 'STATE', true);
            const match = state.match(/PRESET=([^\r\n|]+)/);
            if (match && path.basename(match[1].trim()).toLowerCase() === target) return true;
          } catch { /* pipe may be recovering */ }
          await new Promise(r => setTimeout(r, 500));
        }
        return false;
      };

      // Helper: attempt capture, restart visualizer on failure, retry once
      const captureWithRetry = async (pipe, label, logLines) => {
        let currentPipe = pipe;
        const result = await captureWithPath(currentPipe.path).catch(e => ({ error: e.message }));
        if (!result.error && result.filePath) return { ...result, pipe: currentPipe };

        // Capture failed — restart and retry
        logLines.push(`${label}: capture failed (${result.error}), restarting...`);
        const newPipe = await restartVisualizer(currentPipe, (msg) => logLines.push(msg));
        if (!newPipe) return { error: `restart failed for ${label}`, pipe: currentPipe };

        // If we had a preset, reload it on the restarted instance
        if (preset) {
          const resolvedPreset = await resolvePresetPath(preset, newPipe.path);
          await sendPipeMessage(newPipe.path, `PRESET=${resolvedPreset}`, false).catch(() => {});
          await verifyPresetLoaded(newPipe.path, path.basename(resolvedPreset), 8000);
        }

        const retry = await captureWithPath(newPipe.path).catch(e => ({ error: e.message }));
        return { ...retry, pipe: newPipe };
      };

      // Load preset on both simultaneously if specified
      if (preset) {
        const resolvedPreset = await resolvePresetPath(preset, mdropPipe.path);
        const presetCmd = `PRESET=${resolvedPreset}`;
        await Promise.all([
          sendPipeMessage(mdropPipe.path, presetCmd, false).catch(() => {}),
          sendPipeMessage(milkwavePipe.path, presetCmd, false).catch(() => {}),
        ]);
        results.push({ type: 'text', text: `Loading "${preset}" on both visualizers...\n  Path: ${resolvedPreset}` });

        // Wait for initial settle time
        const waitMs = ((delay || 2) * 1000);
        await new Promise(r => setTimeout(r, waitMs));

        // Verify preset is loaded on both before capturing
        const presetBase = path.basename(resolvedPreset);
        const [mdropReady, mwReady] = await Promise.all([
          verifyPresetLoaded(mdropPipe.path, presetBase, 8000),
          verifyPresetLoaded(milkwavePipe.path, presetBase, 8000),
        ]);
        if (!mdropReady) results.push({ type: 'text', text: `Warning: ${primaryLabel} may not have loaded the preset` });
        if (!mwReady) results.push({ type: 'text', text: `Warning: ${secondaryLabel} may not have loaded the preset` });
      }

      // Query state from both simultaneously
      const [mdropState, milkwaveState] = await Promise.all([
        sendPipeMessage(mdropPipe.path, 'STATE', true).catch(e => `(error: ${e.message})`),
        sendPipeMessage(milkwavePipe.path, 'STATE', true).catch(e => `(error: ${e.message})`),
      ]);
      const maxStateLen = 1000;
      const truncState = (s) => s.length > maxStateLen ? s.slice(0, maxStateLen) + '\n...(truncated)' : s;
      results.push({ type: 'text', text: `--- ${primaryLabel} State ---\n${truncState(mdropState)}` });
      results.push({ type: 'text', text: `--- ${secondaryLabel} State ---\n${truncState(milkwaveState)}` });

      // Capture from both with restart-on-failure
      const captureLog = [];
      const targets = [
        { pipe: mdropPipe, label: primaryLabel, tag: mdropPipe.type },
        { pipe: milkwavePipe, label: secondaryLabel, tag: milkwavePipe.type },
      ];
      const captures = [];
      for (const t of targets) {
        const result = await captureWithRetry(t.pipe, t.label, captureLog);
        captures.push({ ...t, ...result });
      }
      if (captureLog.length > 0) {
        results.push({ type: 'text', text: `Capture log:\n  ${captureLog.join('\n  ')}` });
      }

      // Generate timestamp for comparison filenames
      const ts = new Date().toISOString().replace(/[:.]/g, '-').slice(0, 19);
      const presetSlug = preset
        ? path.basename(preset, path.extname(preset)).replace(/[^a-zA-Z0-9_-]/g, '_').slice(0, 40)
        : 'current';

      const compFiles = [];
      for (const c of captures) {
        if (c.filePath) {
          const outName = `${ts}_${presetSlug}_${c.tag}.jpg`;
          const outPath = downsizeTo(c.filePath, outName, 800, 80);
          if (outPath) {
            compFiles.push(`  ${c.label}: ${outPath}`);
          } else {
            compFiles.push(`  ${c.label}: downsize failed (original: ${c.filePath})`);
          }
        } else {
          compFiles.push(`  ${c.label}: ${c.error || 'capture failed'}`);
        }
      }

      results.push({ type: 'text', text: `Comparison saved to ${comparisonDir}:\n${compFiles.join('\n')}\n\nUse the Read tool on the .jpg files to view them.` });

      return { content: [...results] };
    } catch (err) {
      return { content: [{ type: 'text', text: `Error: ${err.message}` }] };
    }
  }
);

// Tool: Raw command (escape hatch)
server.tool(
  'mdrop_command',
  'Send a raw IPC command to the visualizer (advanced). Use target="all" to send to all running visualizers simultaneously (e.g. SET_AUDIO_GAIN=2.0).',
  {
    command: z.string().describe('Raw pipe command (e.g. "SIGNAL|NEXT_PRESET", "SET_AUDIO_GAIN=2.0")'),
    expect_response: z.boolean().optional().describe('Whether to wait for a response (default: false)'),
    target: z.enum(['auto', 'all', 'mdrop', 'milkwave']).optional().default('auto')
      .describe('Target: "auto" (default, prefers MDropDX12), "all" (all running), "mdrop", or "milkwave"'),
  },
  async ({ command, expect_response, target }) => {
    try {
      if (target === 'all') {
        const allPipes = discoverPipes('all');
        if (allPipes.length === 0) {
          return { content: [{ type: 'text', text: 'No running visualizers found' }] };
        }
        const results = [];
        for (const pipe of allPipes) {
          const label = pipeLabel(pipe.type);
          try {
            const response = await sendPipeMessage(pipe.path, command, expect_response ?? false);
            results.push(`${label}: ${response || 'OK'}`);
          } catch (err) {
            results.push(`${label}: error - ${err.message}`);
          }
        }
        return { content: [{ type: 'text', text: results.join('\n') }] };
      } else {
        // Single target: use discoverPipes with target filter
        const pipes = discoverPipes(target === 'auto' ? 'auto' : target);
        if (pipes.length === 0) {
          return { content: [{ type: 'text', text: `No ${target} visualizer found` }] };
        }
        const response = await sendPipeMessage(pipes[0].path, command, expect_response ?? false);
        return { content: [{ type: 'text', text: response || 'Command sent' }] };
      }
    } catch (err) {
      return { content: [{ type: 'text', text: `Error: ${err.message}` }] };
    }
  }
);

// ── Start server ──

const transport = new StdioServerTransport();
await server.connect(transport);
