// Shadertoy → MDropDX12 shader_import JSON exporter
// Usage: Open a shader on shadertoy.com, open browser DevTools console (F12),
//        paste this entire script and press Enter.
//        The JSON will be copied to clipboard and downloaded as a .json file.
//
// Uses gShaderToy.Save() which returns the Shadertoy native JSON format,
// then converts it to our shader_import format.

(function() {
    'use strict';

    if (typeof gShaderToy === 'undefined') {
        console.error('ERROR: gShaderToy not found. Make sure you are on a Shadertoy shader page.');
        return;
    }

    // gShaderToy.Save() returns { info: {...}, renderpass: [...] }
    const data = gShaderToy.Save();
    if (!data || !data.renderpass || data.renderpass.length === 0) {
        console.error('ERROR: No render passes found in gShaderToy.Save()');
        return;
    }

    // MDropDX12 ChannelSource enum values
    const CHAN = {
        NOISE_LQ:    0,  NOISE_MQ:    1,  NOISE_HQ:    2,
        FEEDBACK:    3,  NOISEVOL_LQ: 4,  NOISEVOL_HQ: 5,
        IMAGE_PREV:  6,  AUDIO:       7,  RANDOM_TEX:  8,
        BUFFER_B:    9,  BUFFER_C:   10,  BUFFER_D:   11,
    };

    // Shadertoy buffer output IDs → buffer index (A=0, B=1, C=2, D=3)
    const BUFFER_ID = { 257: 0, 258: 1, 259: 2, 260: 3 };
    const BUFFER_NAMES = ['Buffer A', 'Buffer B', 'Buffer C', 'Buffer D'];
    const BUFFER_CHAN  = [CHAN.FEEDBACK, CHAN.BUFFER_B, CHAN.BUFFER_C, CHAN.BUFFER_D];

    // Map a Shadertoy input to our ChannelSource enum
    function resolveChannel(input, currentPassName) {
        if (!input) return CHAN.NOISE_LQ;

        const ctype = input.ctype;
        const id = typeof input.id === 'string' ? parseInt(input.id) : input.id;
        const src = (input.src || input.filepath || '').toLowerCase();

        // Buffer references: id 257=A, 258=B, 259=C, 260=D
        if (ctype === 'buffer' && id in BUFFER_ID) {
            return BUFFER_CHAN[BUFFER_ID[id]];
        }

        // Keyboard — no equivalent, map to noise LQ
        if (ctype === 'keyboard') return CHAN.NOISE_LQ;

        // Audio / microphone / music
        if (ctype === 'music' || ctype === 'musicstream' || ctype === 'mic')
            return CHAN.AUDIO;

        // Textures
        if (ctype === 'texture') {
            if (src.includes('medium') || src.includes('rgba01')) return CHAN.NOISE_MQ;
            if (src.includes('small') || src.includes('rgba00')) return CHAN.NOISE_LQ;
            if (src.includes('noise') || src.includes('rgba')) return CHAN.NOISE_HQ;
            if (src.includes('organic') || src.includes('abstract')) return CHAN.RANDOM_TEX;
            return CHAN.NOISE_LQ;
        }

        // Volume/3D textures
        if (ctype === 'volume') {
            if (src.includes('grey') || src.includes('gray')) return CHAN.NOISEVOL_LQ;
            return CHAN.NOISEVOL_HQ;
        }

        // Cubemap fallback
        if (ctype === 'cubemap') return CHAN.NOISE_HQ;

        return CHAN.NOISE_LQ;
    }

    // Determine our pass name from Shadertoy renderpass entry
    function getPassName(rp) {
        if (rp.type === 'image') return 'Image';
        if (rp.type === 'common') return 'Common';
        if (rp.type === 'buffer') {
            // Use the output ID to determine which buffer
            if (rp.outputs && rp.outputs.length > 0) {
                const outId = typeof rp.outputs[0].id === 'string'
                    ? parseInt(rp.outputs[0].id) : rp.outputs[0].id;
                if (outId in BUFFER_ID) return BUFFER_NAMES[BUFFER_ID[outId]];
            }
            // Fallback: try the name field ("Buf A" → "Buffer A")
            const n = (rp.name || '').trim();
            if (n.startsWith('Buf ')) return 'Buffer ' + n.charAt(4);
            return null;
        }
        return null; // skip sound, cubemap, etc.
    }

    // Build output passes
    const passes = [];
    for (const rp of data.renderpass) {
        const name = getPassName(rp);
        if (!name) continue;

        const glsl = rp.code || '';
        const notes = rp.name || '';

        // Parse channel inputs
        const channels = {};
        if (rp.inputs) {
            for (const inp of rp.inputs) {
                const ch = inp.channel;
                if (ch >= 0 && ch <= 3) {
                    channels['ch' + ch] = resolveChannel(inp, name);
                }
            }
        }
        // Fill defaults for unassigned channels
        for (let i = 0; i < 4; i++) {
            const key = 'ch' + i;
            if (!(key in channels)) {
                channels[key] = [CHAN.NOISE_LQ, CHAN.NOISE_LQ, CHAN.NOISE_MQ, CHAN.NOISE_HQ][i];
            }
        }

        passes.push({ name, glsl, notes, channels });
    }

    // Reorder: Image, Common, Buffer A, B, C, D
    const order = ['Image', 'Common', 'Buffer A', 'Buffer B', 'Buffer C', 'Buffer D'];
    passes.sort((a, b) => order.indexOf(a.name) - order.indexOf(b.name));

    const output = { type: 'shader_import', version: 1, passes };
    const json = JSON.stringify(output, null, 2);

    // Summary
    const shaderName = data.info ? data.info.name : 'unknown';
    console.log(`%cShadertoy Export: ${shaderName}`, 'color: #0af; font-weight: bold; font-size: 14px');
    console.log(`%c${passes.length} passes: ${passes.map(p => p.name).join(', ')}`, 'color: #aaa');
    console.log(`%cGLSL sizes: ${passes.map(p => p.name + '=' + p.glsl.length + ' chars').join(', ')}`, 'color: #aaa');

    // Warn about empty passes
    const empty = passes.filter(p => p.name !== 'Common' && p.glsl.length === 0);
    if (empty.length > 0) {
        console.warn(`%c⚠ Empty GLSL in: ${empty.map(p => p.name).join(', ')}`, 'color: #ff0; font-weight: bold');
    }

    // Copy to clipboard
    navigator.clipboard.writeText(json).then(() => {
        console.log('%c✓ JSON copied to clipboard!', 'color: #0f0; font-weight: bold; font-size: 14px');
    }).catch(() => {
        console.log('%c✗ Could not copy to clipboard — use the download instead', 'color: #f00; font-weight: bold');
    });

    // Trigger download
    const blob = new Blob([json], { type: 'application/json' });
    const url = URL.createObjectURL(blob);
    const a = document.createElement('a');
    a.href = url;
    const safeName = (shaderName || 'shader').replace(/[^a-zA-Z0-9_-]/g, '');
    a.download = safeName + '.json';
    document.body.appendChild(a);
    a.click();
    document.body.removeChild(a);
    URL.revokeObjectURL(url);
    console.log(`%c✓ Downloaded: ${a.download}`, 'color: #0f0');

    // Log full JSON for manual copy
    console.log(json);
})();
