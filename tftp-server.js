#!/usr/bin/env node
const path = require('path');
const dgram = require('dgram');
const https = require('https');
const { execSync } = require('child_process');

const TFTPROOT = path.join(__dirname, 'tftproot');
const REPO = 'AnEntrypoint/looper';
const SERVER_IP = '192.168.137.1';
const PI_IP = '192.168.137.100';
const POOL_START = [192,168,137,100];
const SUBNET = [255,255,255,0];
const LEASE_SECS = 3600;
const SHA_FILE = path.join(__dirname, '.tftp-sha');
const LOG_FILE = path.join(__dirname, 'syslog.log');

const OP_RRQ=1,OP_DATA=3,OP_ACK=4,OP_ERR=5,OP_OACK=6;
let currentSha=null, rateLimitedUntil=0;
try { currentSha=fs.readFileSync(SHA_FILE,'utf8').trim(); console.log('[UPDATE] sha:',currentSha); } catch(e){}
const GH_HEADERS={'User-Agent':'looper-tftp/1.0'};
if (process.env.GITHUB_TOKEN) GH_HEADERS['Authorization']='token '+process.env.GITHUB_TOKEN;
const leases={};

function ip2buf(str){return Buffer.from(str.split('.').map(Number));}
function allocate(mac){if(leases[mac])return leases[mac];const ip=[...POOL_START];ip[3]+=Object.keys(leases).length;return leases[mac]=ip.join('.');}

function httpsGet(url){
  return new Promise((res,rej)=>{
    https.get(url,{headers:GH_HEADERS},r=>{
      if(r.statusCode===301||r.statusCode===302)return httpsGet(r.headers.location).then(res).catch(rej);
      let d='';r.on('data',c=>d+=c);r.on('end',()=>res({status:r.statusCode,body:d,headers:r.headers}));
    }).on('error',rej);
  });
}

function downloadFile(url,dest){
  return new Promise((res,rej)=>{
    const follow=u=>{
      https.get(u,{headers:{'User-Agent':'looper-tftp/1.0'}},r=>{
        if(r.statusCode===301||r.statusCode===302)return follow(r.headers.location);
        const tmp=dest+'.tmp',out=fs.createWriteStream(tmp);
        r.pipe(out);out.on('finish',()=>{fs.renameSync(tmp,dest);res();});out.on('error',rej);
      }).on('error',rej);
    };follow(url);
  });
}

async function checkAndUpdate(){
  try{
    if(Date.now()<rateLimitedUntil){console.log('[UPDATE] rate-limited');return;}
    const r=await httpsGet('https://api.github.com/repos/'+REPO+'/releases/latest');
    if(r.status===403||r.status===429){rateLimitedUntil=Date.now()+(parseInt(r.headers['retry-after']||60)*1000);console.error('[UPDATE] rate-limited');return;}
    if(r.status!==200){console.log('[UPDATE] GitHub status',r.status);return;}
    const release=JSON.parse(r.body);
    const asset=release.assets.find(a=>a.name==='looper-sd.zip');
    if(!asset){console.log('[UPDATE] no looper-sd.zip in',release.tag_name);return;}
    const sha=asset.updated_at||String(release.id);
    if(sha===currentSha){console.log('[UPDATE] up to date',release.tag_name);return;}
    console.log('[UPDATE] new build',release.tag_name,sha);
    const zipPath=path.join(__dirname,'dist','looper-sd.zip');
    const extractDir=path.join(__dirname,'dist','looper-sd');
    fs.mkdirSync(path.dirname(zipPath),{recursive:true});
    await downloadFile(asset.browser_download_url,zipPath);
    fs.rmSync(extractDir,{recursive:true,force:true});
    execSync('powershell -command "Expand-Archive -Path \''+zipPath+'\' -DestinationPath \''+extractDir+'\' -Force"',{stdio:'pipe'});
    const findKernel=dir=>{for(const e of fs.readdirSync(dir)){const p=path.join(dir,e);if(e==='kernel7l.img')return p;if(fs.statSync(p).isDirectory()){const r=findKernel(p);if(r)return r;}}return null;};
    const kernelSrc=findKernel(extractDir);
    if(!kernelSrc){console.error('[UPDATE] kernel7l.img not found in zip');return;}
    const kernelBuf=fs.readFileSync(kernelSrc);
    fs.writeFileSync(path.join(TFTPROOT,'kernel7l.img'),kernelBuf);
    for(const entry of fs.readdirSync(TFTPROOT)){
      const sub=path.join(TFTPROOT,entry);
      if(fs.statSync(sub).isDirectory()){const dest=path.join(sub,'kernel7l.img');if(fs.existsSync(dest)){fs.writeFileSync(dest,kernelBuf);console.log('[UPDATE] synced ->',entry+'/');}}
    }
    currentSha=sha;fs.writeFileSync(SHA_FILE,sha);
    console.log('[UPDATE] tftproot updated',kernelBuf.length,'bytes');
    const sock=dgram.createSocket('udp4');
    sock.send(Buffer.from('REBOOT'),0,6,4444,PI_IP,()=>{sock.close();console.log('[UPDATE] REBOOT sent');});
  }catch(e){console.error('[UPDATE] failed:',e.message);}
}

function parseOpts(buf,offset){const opts={};while(offset<buf.length){const end=buf.indexOf(0,offset);if(end===-1)break;const k=buf.slice(offset,end).toString().toLowerCase();offset=end+1;const e2=buf.indexOf(0,offset);if(e2===-1)break;opts[k]=buf.slice(offset,e2).toString();offset=e2+1;}return opts;}
function buildOACK(opts){const p=[Buffer.from([0,OP_OACK])];for(const[k,v]of Object.entries(opts))p.push(Buffer.from(k+'\0'+v+'\0'));return Buffer.concat(p);}

function handleRRQ(filename,rinfo,options){
  const safe=path.normalize(filename).replace(/^(\.\.[/\\])+/,'');
  const full=path.join(TFTPROOT,safe);
  if(!full.startsWith(TFTPROOT))return;
  const xfer=dgram.createSocket('udp4');
  xfer.bind(0,()=>{
    if(!fs.existsSync(full)){
      const e=Buffer.alloc(4+15);e.writeUInt16BE(OP_ERR,0);e.writeUInt16BE(1,2);Buffer.from('File not found').copy(e,4);
      xfer.send(e,rinfo.port,rinfo.address);setTimeout(()=>xfer.close(),500);
      console.error('[TFTP] NOT FOUND:',safe);return;
    }
    const data=fs.readFileSync(full);
    const blksize=options.blksize?parseInt(options.blksize):512;
    const replyOpts={};
    if(options.blksize)replyOpts.blksize=String(blksize);
    if(options.tsize)replyOpts.tsize=String(data.length);
    const blocks=Math.ceil(data.length/blksize)||1;
    let acked=options.blksize||options.tsize?-1:0;
    if(Object.keys(replyOpts).length)xfer.send(buildOACK(replyOpts),rinfo.port,rinfo.address);
    else{const d=Buffer.alloc(4+Math.min(blksize,data.length));d.writeUInt16BE(OP_DATA,0);d.writeUInt16BE(1,2);data.copy(d,4,0,blksize);xfer.send(d,rinfo.port,rinfo.address);acked=0;}
    console.log('[TFTP]',safe,'->',rinfo.address,data.length+'B');
    xfer.on('message',msg=>{
      if(msg.readUInt16BE(0)!==OP_ACK)return;
      const blk=msg.readUInt16BE(2);
      if(blk!==acked+1&&!(blk===0&&acked===-1))return;
      acked=blk;
      if(acked>=blocks){xfer.close();return;}
      const start=acked*blksize,chunk=data.slice(start,start+blksize);
      const pkt=Buffer.alloc(4+chunk.length);pkt.writeUInt16BE(OP_DATA,0);pkt.writeUInt16BE((acked+1)&0xffff,2);chunk.copy(pkt,4);
      xfer.send(pkt,rinfo.port,rinfo.address);
    });
  });
}

function parseSyslog(raw){
  let msg=raw.toString();
  const pri=msg.match(/^<\d+>(.*)/s);if(pri)msg=pri[1];
  const c=msg.match(/^\d+\s+-\s+\S+\s+(\S+)\s+-\s+-\s+-\s+(.*)/s);
  if(c)msg=c[1]+': '+c[2];else msg=msg.replace(/^\w{3}\s+\d+\s+\d+:\d+:\d+\s+\S+\s+/,'');
  return msg.replace(/[^\x20-\x7e\n]/g,'').trim();
}

function buildDhcpReply(type,xid,mac,offeredIp){
  const buf=Buffer.alloc(576);
  buf[0]=2;buf[1]=1;buf[2]=6;buf.writeUInt32BE(xid,4);buf.writeUInt16BE(0x8000,10);
  ip2buf(offeredIp).copy(buf,16);ip2buf(SERVER_IP).copy(buf,20);mac.copy(buf,28);
  buf.writeUInt32BE(0x63825363,236);
  let o=240;
  buf[o++]=53;buf[o++]=1;buf[o++]=type;
  buf[o++]=54;buf[o++]=4;ip2buf(SERVER_IP).copy(buf,o);o+=4;
  buf[o++]=51;buf[o++]=4;buf.writeUInt32BE(LEASE_SECS,o);o+=4;
  buf[o++]=1;buf[o++]=4;Buffer.from(SUBNET).copy(buf,o);o+=4;
  const ti=Buffer.from(SERVER_IP);buf[o++]=66;buf[o++]=ti.length;ti.copy(buf,o);o+=ti.length;
  const boot=Buffer.from('kernel7l.img\0');buf[o++]=67;buf[o++]=boot.length;boot.copy(buf,o);o+=boot.length;
  buf[o++]=255;
  return buf.slice(0,o);
}

const tftp=dgram.createSocket('udp4');
tftp.on('message',(msg,rinfo)=>{
  if(msg.readUInt16BE(0)!==OP_RRQ)return;
  let offset=2;const end=msg.indexOf(0,offset);
  const filename=msg.slice(offset,end).toString();offset=end+1;
  const modeEnd=msg.indexOf(0,offset);offset=modeEnd+1;
  const opts=parseOpts(msg,offset);
  handleRRQ(filename,rinfo,opts);
});
tftp.on('error',err=>{if(err.code==='EACCES'){console.error('[TFTP] need admin: port 69');process.exit(1);}console.error('[TFTP]',err.message);});
tftp.bind(69,'0.0.0.0',()=>console.log('[TFTP] listening :69 root='+TFTPROOT));

const dhcp=dgram.createSocket({type:'udp4',reuseAddr:true});
dhcp.on('message',(msg,rinfo)=>{
  if(msg.length<240||msg[0]!==1||msg.readUInt32BE(236)!==0x63825363)return;
  const xid=msg.readUInt32BE(4),mac=msg.slice(28,34);
  const macStr=Array.from(mac).map(b=>b.toString(16).padStart(2,'0')).join(':');
  let msgType=0,o=240;
  while(o<msg.length){const opt=msg[o++];if(opt===255)break;if(opt===0)continue;const len=msg[o++];if(opt===53)msgType=msg[o];o+=len;}
  const offeredIp=allocate(macStr);
  console.log('[DHCP]',msgType===1?'DISCOVER':msgType===3?'REQUEST':'type'+msgType,'from',macStr,'->',offeredIp);
  const reply=buildDhcpReply(msgType===1?2:5,xid,mac,offeredIp);
  dhcp.send(reply,68,'255.255.255.255',err=>err&&console.error('[DHCP]',err.message));
});
dhcp.on('error',err=>{if(err.code==='EACCES'){console.error('[DHCP] need admin: port 67');process.exit(1);}console.error('[DHCP]',err.message);});
dhcp.bind(67,'0.0.0.0',()=>{dhcp.setBroadcast(true);console.log('[DHCP] listening :67 tftp='+SERVER_IP);});

const syslog=dgram.createSocket('udp4');
syslog.on('message',(msg,rinfo)=>{
  const text=parseSyslog(msg.toString());
  if(!text||text.startsWith('icmp:'))return;
  const line='['+new Date().toISOString().substring(11,23)+'] '+text+'\n';
  process.stdout.write(line);fs.appendFileSync(LOG_FILE,line);
});
syslog.on('error',err=>{if(err.code==='EACCES'){console.error('[syslog] need admin: port 514');process.exit(1);}console.error('[syslog]',err.message);});
syslog.bind(514,'0.0.0.0',()=>console.log('[syslog] listening :514'));

checkAndUpdate();
setInterval(checkAndUpdate,30000);
