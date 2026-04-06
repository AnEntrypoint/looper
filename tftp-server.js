#!/usr/bin/env node
// tftp-server.js — serves tftproot/ over TFTP for rPi4 netboot
// Handles tsize/blksize options (OACK) required by rPi4 bootloader
// Auto-updates kernel7l.img from latest GitHub release every 30s
// Usage: node tftp-server.js [port=69]

const path = require('path');
const fs = require('fs');
const dgram = require('dgram');
const https = require('https');
const { execSync } = require('child_process');

const TFTPROOT = path.join(__dirname, 'tftproot');
const PORT = parseInt(process.argv[2] || '69');
const REPO = 'AnEntrypoint/looper';
const CHECK_INTERVAL_MS = 30000;

const OP_RRQ  = 1;
const OP_DATA = 3;
const OP_ACK  = 4;
const OP_ERR  = 5;
const OP_OACK = 6;

// ── Auto-update ────────────────────────────────────────────────────────────

let currentSha = null;

function httpsGet(url) {
  return new Promise((res, rej) => {
    https.get(url, { headers: { 'User-Agent': 'looper-tftp/1.0' } }, r => {
      if (r.statusCode === 302 || r.statusCode === 301) return httpsGet(r.headers.location).then(res).catch(rej);
      let d = ''; r.on('data', c => d += c); r.on('end', () => res({ status: r.statusCode, body: d }));
    }).on('error', rej);
  });
}

function downloadFile(url, dest) {
  return new Promise((res, rej) => {
    const follow = (u) => {
      https.get(u, { headers: { 'User-Agent': 'looper-tftp/1.0' } }, r => {
        if (r.statusCode === 302 || r.statusCode === 301) return follow(r.headers.location);
        const tmp = dest + '.tmp';
        const out = fs.createWriteStream(tmp);
        r.pipe(out);
        out.on('finish', () => { fs.renameSync(tmp, dest); res(); });
        out.on('error', rej);
      }).on('error', rej);
    };
    follow(url);
  });
}

async function checkAndUpdate() {
  try {
    const r = await httpsGet(`https://api.github.com/repos/${REPO}/releases/latest`);
    if (r.status !== 200) return;
    const release = JSON.parse(r.body);
    const sha = release.target_commitish || release.id;
    if (sha === currentSha) return;
    const asset = release.assets.find(a => a.name === 'looper-sd.zip');
    if (!asset) return;
    console.log(`[UPDATE] New build detected (${release.tag_name}), downloading...`);
    const zipPath = path.join(__dirname, 'dist', 'looper-sd.zip');
    fs.mkdirSync(path.dirname(zipPath), { recursive: true });
    await downloadFile(asset.browser_download_url, zipPath);
    execSync(`powershell -command "Expand-Archive -Path '${zipPath}' -DestinationPath '${TFTPROOT}' -Force"`, { stdio: 'pipe' });
    currentSha = sha;
    console.log(`[UPDATE] tftproot updated — kernel7l.img ${fs.statSync(path.join(TFTPROOT,'kernel7l.img')).size} bytes`);
  } catch (e) {
    console.error('[UPDATE] Check failed:', e.message);
  }
}

// ── TFTP ───────────────────────────────────────────────────────────────────

function parseOptions(buf, offset) {
  const opts = {};
  while (offset < buf.length) {
    const end = buf.indexOf(0, offset);
    if (end === -1) break;
    const key = buf.slice(offset, end).toString().toLowerCase();
    offset = end + 1;
    const end2 = buf.indexOf(0, offset);
    if (end2 === -1) break;
    const val = buf.slice(offset, end2).toString();
    offset = end2 + 1;
    opts[key] = val;
  }
  return opts;
}

function buildOACK(opts) {
  // OP_OACK + null-terminated key=value pairs
  const parts = [Buffer.from([0, OP_OACK])];
  for (const [k, v] of Object.entries(opts)) {
    parts.push(Buffer.from(k + '\0' + v + '\0'));
  }
  return Buffer.concat(parts);
}

function handleRRQ(filename, rinfo, options) {
  const safeName = path.normalize(filename).replace(/^(\.\.[/\\])+/, '');
  const fullPath = path.join(TFTPROOT, safeName);
  if (!fullPath.startsWith(TFTPROOT)) return;

  if (!fs.existsSync(fullPath)) {
    const xfer = dgram.createSocket('udp4');
    xfer.bind(0, () => {
      const e = Buffer.alloc(5 + safeName.length);
      e.writeUInt16BE(OP_ERR,0); e.writeUInt16BE(1,2);
      Buffer.from('File not found').copy(e,4);
      xfer.send(e, rinfo.port, rinfo.address);
      setTimeout(()=>xfer.close(),500);
    });
    console.error(`[TFTP] NOT FOUND: ${safeName} from ${rinfo.address}`);
    return;
  }

  const data = fs.readFileSync(fullPath);
  const fileSize = data.length;

  // Negotiate options
  const negotiated = {};
  let blksize = 512;
  if (options.blksize) { blksize = Math.min(parseInt(options.blksize), 1468); negotiated.blksize = String(blksize); }
  if (options.tsize)   { negotiated.tsize = String(fileSize); }

  const totalBlocks = Math.ceil(fileSize / blksize) || 1;
  console.log(`[TFTP] ${rinfo.address} <- ${safeName} (${fileSize} bytes, blksize=${blksize}, ${totalBlocks} blocks)`);

  const xfer = dgram.createSocket('udp4');
  let block = 0; // 0 = waiting for ACK of OACK, then start at block 1
  let retries = 0;
  let timer = null;

  function sendBlock() {
    if (block === 0) {
      if (Object.keys(negotiated).length > 0) {
        const oack = buildOACK(negotiated);
        xfer.send(oack, rinfo.port, rinfo.address);
      } else {
        block = 1;
        sendBlock();
        return;
      }
    } else {
      const start = (block - 1) * blksize;
      const chunk = data.slice(start, start + blksize);
      const pkt = Buffer.alloc(4 + chunk.length);
      pkt.writeUInt16BE(OP_DATA, 0);
      pkt.writeUInt16BE(block & 0xFFFF, 2);
      chunk.copy(pkt, 4);
      xfer.send(pkt, rinfo.port, rinfo.address);
    }
    if (timer) clearTimeout(timer);
    timer = setTimeout(() => {
      if (++retries > 5) { xfer.close(); console.error(`[TFTP] Timeout: ${safeName} block ${block}`); return; }
      sendBlock();
    }, 2000);
  }

  xfer.bind(0, () => sendBlock());

  xfer.on('message', (msg) => {
    const op = msg.readUInt16BE(0);
    if (op !== OP_ACK) return;
    const ack = msg.readUInt16BE(2);
    // ACK 0 = OACK acknowledged
    const expected = block & 0xFFFF;
    if (ack !== expected) return;
    retries = 0;
    if (block > 0 && block >= totalBlocks) {
      if (timer) clearTimeout(timer);
      xfer.close();
      console.log(`[TFTP] Done: ${safeName} -> ${rinfo.address}`);
      return;
    }
    block++;
    sendBlock();
  });

  xfer.on('error', () => { if (timer) clearTimeout(timer); });
}

const server = dgram.createSocket({type:'udp4', reuseAddr:true});

server.on('message', (msg, rinfo) => {
  const op = msg.readUInt16BE(0);
  if (op !== OP_RRQ) return;
  let offset = 2;
  const fnEnd = msg.indexOf(0, offset);
  const filename = msg.slice(offset, fnEnd).toString();
  offset = fnEnd + 1;
  const modeEnd = msg.indexOf(0, offset);
  offset = modeEnd + 1;
  const options = parseOptions(msg, offset);
  if (Object.keys(options).length) console.log(`[TFTP] ${rinfo.address} RRQ ${filename} opts:`, options);
  handleRRQ(filename, rinfo, options);
});

server.on('error', (err) => {
  if (err.code === 'EACCES') { console.error(`[TFTP] Need admin for port ${PORT}`); process.exit(1); }
  console.error('[TFTP] Error:', err.message);
});

server.bind(PORT, '0.0.0.0', async () => {
  console.log(`[TFTP] Listening on port ${PORT}`);
  console.log(`[TFTP] Serving: ${TFTPROOT}`);
  console.log(`[TFTP] Checking for updates every ${CHECK_INTERVAL_MS/1000}s`);
  await checkAndUpdate();
  setInterval(checkAndUpdate, CHECK_INTERVAL_MS);
});
