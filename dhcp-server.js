#!/usr/bin/env node
// dhcp-server.js — minimal DHCP server for rPi4 netboot
// Sends option 66 (TFTP server) so rPi4 knows where to fetch kernel
// Usage: node dhcp-server.js

const dgram = require('dgram');

const SERVER_IP  = '192.168.137.1';
const POOL_START = [192,168,137,100];
const SUBNET     = [255,255,255,0];
const LEASE_SECS = 3600;

function ip2buf(str) { return Buffer.from(str.split('.').map(Number)); }
function buf2ip(b, o=0) { return `${b[o]}.${b[o+1]}.${b[o+2]}.${b[o+3]}`; }

const leases = {}; // mac -> ip

function allocate(mac) {
  if (leases[mac]) return leases[mac];
  const idx = Object.keys(leases).length;
  const ip = [...POOL_START]; ip[3] += idx;
  leases[mac] = ip.join('.');
  return leases[mac];
}

function buildReply(type, xid, clientMac, offeredIp) {
  const buf = Buffer.alloc(576);
  buf[0] = 2; // BOOTREPLY
  buf[1] = 1; buf[2] = 6; // Ethernet, 6-byte MAC
  buf[7] = 0; // hops
  buf.writeUInt32BE(xid, 4);
  buf.writeUInt16BE(0, 8);  // secs
  buf.writeUInt16BE(0x8000, 10); // flags: broadcast
  ip2buf(offeredIp).copy(buf, 16); // yiaddr
  ip2buf(SERVER_IP).copy(buf, 20); // siaddr
  ip2buf(SERVER_IP).copy(buf, 28); // sname used as next-server
  Buffer.from(SERVER_IP).copy(buf, 44); // sname field (string)
  clientMac.copy(buf, 28+16); // chaddr at offset 28? No: chaddr is at 28
  // chaddr is at offset 28 in DHCP? No: op(1)+htype(1)+hlen(1)+hops(1)+xid(4)+secs(2)+flags(2)+ciaddr(4)+yiaddr(4)+siaddr(4)+giaddr(4) = 28, then chaddr(16)
  clientMac.copy(buf, 28);
  // Magic cookie
  buf.writeUInt32BE(0x63825363, 236);
  let o = 240;
  // Option 53: DHCP message type
  buf[o++]=53; buf[o++]=1; buf[o++]=type;
  // Option 54: server identifier
  buf[o++]=54; buf[o++]=4; ip2buf(SERVER_IP).copy(buf,o); o+=4;
  // Option 51: lease time
  buf[o++]=51; buf[o++]=4; buf.writeUInt32BE(LEASE_SECS,o); o+=4;
  // Option 1: subnet mask
  buf[o++]=1; buf[o++]=4; Buffer.from(SUBNET).copy(buf,o); o+=4;
  // Option 66: TFTP server name
  const tftpIp = Buffer.from(SERVER_IP);
  buf[o++]=66; buf[o++]=tftpIp.length; tftpIp.copy(buf,o); o+=tftpIp.length;
  // Option 67: bootfile name (rPi4 requests this)
  const boot = Buffer.from('kernel7l.img\0');
  buf[o++]=67; buf[o++]=boot.length; boot.copy(buf,o); o+=boot.length;
  // End
  buf[o++]=255;
  return buf.slice(0, o);
}

const server = dgram.createSocket({type:'udp4', reuseAddr:true});

server.on('message', (msg, rinfo) => {
  if (msg.length < 240) return;
  if (msg[0] !== 1) return; // only BOOTREQUEST
  const xid = msg.readUInt32BE(4);
  const mac = msg.slice(28, 34);
  const macStr = Array.from(mac).map(b=>b.toString(16).padStart(2,'0')).join(':');

  // Magic cookie check
  if (msg.readUInt32BE(236) !== 0x63825363) return;

  // Parse options to find msg type
  let msgType = 0;
  let o = 240;
  while (o < msg.length) {
    const opt = msg[o++];
    if (opt === 255) break;
    if (opt === 0) continue;
    const len = msg[o++];
    if (opt === 53) msgType = msg[o];
    o += len;
  }

  const offeredIp = allocate(macStr);
  console.log(`[DHCP] ${msgType===1?'DISCOVER':msgType===3?'REQUEST':'type'+msgType} from ${macStr} -> offering ${offeredIp}`);

  const replyType = msgType === 1 ? 2 : 5; // OFFER or ACK
  const reply = buildReply(replyType, xid, mac, offeredIp);
  server.send(reply, 68, '255.255.255.255', (err) => {
    if (err) console.error('[DHCP] send error:', err.message);
    else console.log(`[DHCP] ${replyType===2?'OFFER':'ACK'} sent to ${macStr} ip=${offeredIp} tftp=${SERVER_IP}`);
  });
});

server.on('error', err => {
  if (err.code === 'EACCES') { console.error('[DHCP] Need admin: port 67 requires elevation'); process.exit(1); }
  console.error('[DHCP] Error:', err.message);
});

server.bind(67, '0.0.0.0', () => {
  server.setBroadcast(true);
  console.log('[DHCP] Server listening on 0.0.0.0:67');
  console.log('[DHCP] TFTP server announced as:', SERVER_IP);
});
