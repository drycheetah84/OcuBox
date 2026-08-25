// payload_manifest.mjs — Parse an Android update_engine payload.bin manifest.
// Reads the manifest directly from the OTA zip using the known stored offset
// (no full 1GB extraction). Emits the partition table as JSON + human summary.
//
// Usage: node payload_manifest.mjs <ota.zip> <payloadOffsetInZip> [outJson]
import { open } from 'node:fs/promises';

const OP_TYPES = {
  0: 'REPLACE', 1: 'REPLACE_BZ', 2: 'MOVE', 3: 'BSDIFF', 4: 'SOURCE_COPY',
  5: 'SOURCE_BSDIFF', 6: 'ZERO', 7: 'DISCARD', 8: 'REPLACE_XZ',
  9: 'PUFFDIFF', 10: 'BROTLI_BSDIFF',
};

// ---- Minimal protobuf reader (wire format) ----
class PB {
  constructor(buf) { this.b = buf; this.p = 0; }
  eof() { return this.p >= this.b.length; }
  varint() {
    let shift = 0n, result = 0n;
    while (true) {
      const byte = this.b[this.p++];
      result |= BigInt(byte & 0x7f) << shift;
      if ((byte & 0x80) === 0) break;
      shift += 7n;
    }
    return result;
  }
  tag() { const v = this.varint(); return { field: Number(v >> 3n), wire: Number(v & 7n) }; }
  bytes() { const len = Number(this.varint()); const s = this.b.subarray(this.p, this.p + len); this.p += len; return s; }
  skip(wire) {
    if (wire === 0) this.varint();
    else if (wire === 1) this.p += 8;
    else if (wire === 2) { const len = Number(this.varint()); this.p += len; }
    else if (wire === 5) this.p += 4;
    else throw new Error('bad wire ' + wire);
  }
}

function parsePartitionInfo(buf) {
  const pb = new PB(buf); let size = 0n, hash = null;
  while (!pb.eof()) { const t = pb.tag();
    if (t.field === 1 && t.wire === 0) size = pb.varint();
    else if (t.field === 2 && t.wire === 2) hash = Buffer.from(pb.bytes());
    else pb.skip(t.wire);
  }
  return { size, hash };
}

function parseOperation(buf) {
  const pb = new PB(buf);
  let type = -1, dataOffset = 0n, dataLength = 0n, dstBlocks = 0n;
  while (!pb.eof()) { const t = pb.tag();
    if (t.field === 1 && t.wire === 0) type = Number(pb.varint());
    else if (t.field === 2 && t.wire === 0) dataOffset = pb.varint();
    else if (t.field === 3 && t.wire === 0) dataLength = pb.varint();
    else if (t.field === 6 && t.wire === 2) { // repeated Extent dst_extents
      const ext = new PB(pb.bytes());
      while (!ext.eof()) { const et = ext.tag();
        if (et.field === 2 && et.wire === 0) dstBlocks += ext.varint(); else ext.skip(et.wire); }
    } else pb.skip(t.wire);
  }
  return { type, dataOffset, dataLength, dstBlocks };
}

function parsePartitionUpdate(buf) {
  const pb = new PB(buf);
  let name = '', newInfo = null, ops = [];
  while (!pb.eof()) { const t = pb.tag();
    if (t.field === 1 && t.wire === 2) name = Buffer.from(pb.bytes()).toString('utf8');
    else if (t.field === 7 && t.wire === 2) newInfo = parsePartitionInfo(pb.bytes());
    else if (t.field === 8 && t.wire === 2) ops.push(parseOperation(pb.bytes()));
    else pb.skip(t.wire);
  }
  return { name, newInfo, ops };
}

function parseManifest(buf) {
  const pb = new PB(buf);
  let blockSize = 4096; const partitions = [];
  while (!pb.eof()) { const t = pb.tag();
    if (t.field === 3 && t.wire === 0) blockSize = Number(pb.varint());
    else if (t.field === 13 && t.wire === 2) partitions.push(parsePartitionUpdate(pb.bytes()));
    else pb.skip(t.wire);
  }
  return { blockSize, partitions };
}

async function main() {
  const [zipPath, offStr, outJson] = process.argv.slice(2);
  const payloadOffset = Number(offStr);
  const fh = await open(zipPath, 'r');

  // Read fixed header: magic(4) version(u64) manifest_size(u64) metadata_sig_size(u32)
  const head = Buffer.alloc(24);
  await fh.read(head, 0, 24, payloadOffset);
  const magic = head.toString('ascii', 0, 4);
  if (magic !== 'CrAU') throw new Error('Bad payload magic: ' + magic);
  const version = head.readBigUInt64BE(4);
  const manifestSize = Number(head.readBigUInt64BE(12));
  const metaSigSize = head.readUInt32BE(20);
  const headerSize = 24;

  const manifestBuf = Buffer.alloc(manifestSize);
  await fh.read(manifestBuf, 0, manifestSize, payloadOffset + headerSize);
  await fh.close();

  const { blockSize, partitions } = parseManifest(manifestBuf);

  // dataBlobStart is the offset (within payload.bin) where operation data blobs begin.
  const dataBlobStart = headerSize + manifestSize + metaSigSize;

  const result = {
    payload: { version: Number(version), headerSize, manifestSize, metaSigSize,
               dataBlobStart, blockSize, payloadOffsetInZip: payloadOffset },
    partitions: partitions.map(p => {
      const opTypes = {};
      let dataSpanEnd = 0n, dataSpanStart = null;
      for (const o of p.ops) {
        opTypes[OP_TYPES[o.type] ?? o.type] = (opTypes[OP_TYPES[o.type] ?? o.type] || 0) + 1;
        const end = o.dataOffset + o.dataLength;
        if (o.dataLength > 0n) {
          if (dataSpanStart === null || o.dataOffset < dataSpanStart) dataSpanStart = o.dataOffset;
          if (end > dataSpanEnd) dataSpanEnd = end;
        }
      }
      return {
        name: p.name,
        size: p.newInfo ? Number(p.newInfo.size) : null,
        sha256: p.newInfo?.hash ? p.newInfo.hash.toString('hex') : null,
        numOps: p.ops.length,
        opTypes,
        // For single-op partitions this pins the blob location within the zip:
        firstOp: p.ops[0] ? {
          type: OP_TYPES[p.ops[0].type] ?? p.ops[0].type,
          dataOffsetInZip: payloadOffset + dataBlobStart + Number(p.ops[0].dataOffset),
          dataLength: Number(p.ops[0].dataLength),
        } : null,
      };
    }),
  };

  console.log(`payload version=${result.payload.version} block=${blockSize} manifest=${manifestSize}B metaSig=${metaSigSize}B`);
  console.log(`data blobs start at zip offset ${payloadOffset + dataBlobStart}`);
  console.log(`\n${'PARTITION'.padEnd(20)} ${'SIZE(MB)'.padStart(10)} ${'OPS'.padStart(5)}  OP-TYPES`);
  console.log('-'.repeat(78));
  for (const p of result.partitions.sort((a,b)=>(b.size||0)-(a.size||0))) {
    const mb = p.size != null ? (p.size/1048576).toFixed(2) : '?';
    console.log(`${p.name.padEnd(20)} ${mb.padStart(10)} ${String(p.numOps).padStart(5)}  ${Object.entries(p.opTypes).map(([k,v])=>`${k}:${v}`).join(' ')}`);
  }
  console.log(`\nTotal partitions: ${result.partitions.length}`);

  if (outJson) {
    const { writeFile } = await import('node:fs/promises');
    await writeFile(outJson, JSON.stringify(result, null, 2));
    console.log(`\nWrote ${outJson}`);
  }
}
main().catch(e => { console.error(e); process.exit(1); });
