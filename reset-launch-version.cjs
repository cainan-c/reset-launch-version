#!/usr/bin/env node
// SPDX-License-Identifier: MIT
// Push a selected launch version to AVM and ns:am2. Dry-run is the default.
const fs = require('node:fs/promises');
const path = require('node:path');
const ns2jbRoot = path.resolve(process.env.NS2JB_ROOT || path.resolve(__dirname, '../../..'));
const fromNs2jb = (...parts) => require(path.join(ns2jbRoot, ...parts));
const {Session, live, check} = fromNs2jb('engine', 'switch2', 'app', 'session.cjs');
const {connect} = fromNs2jb('engine', 'switch2', 'tools', 'fs-memory.cjs');
const {runPath} = fromNs2jb('engine', 'shared', 'paths.cjs');

function parseAppId(value) {
  check(typeof value === 'string' && /^(?:0x)?[0-9a-fA-F]{1,16}$/.test(value),
    'Application ID must be a 64-bit hexadecimal ID');
  return BigInt(value.startsWith('0x') ? value : '0x' + value);
}

function parseVersion(value) {
  check(typeof value === 'string' && /^(?:0|0x[0-9a-fA-F]{1,8}|[0-9]{1,10})$/.test(value),
    'Version must be a uint32 decimal or 0x-prefixed hexadecimal value');
  const version = value.startsWith('0x') ? Number.parseInt(value.slice(2), 16) : Number(value);
  check(Number.isInteger(version) && version >= 0 && version <= 0xffffffff, 'Version is outside uint32 range');
  return version;
}

function parseArgs(args) {
  let title = null, all = false, apply = false, version = 0;
  for (let i = 0; i < args.length; i++) {
    const value = args[i];
    if (value === '--all') all = true;
    else if (value === '--apply') apply = true;
    else if (value === '--version' && i + 1 < args.length) version = parseVersion(args[++i]);
    else if (value.startsWith('--')) throw Error('Unknown or incomplete option: ' + value);
    else if (title === null) title = parseAppId(value);
    else throw Error('Unexpected argument: ' + value);
  }
  check((title !== null) !== all,
    'Usage: node reset-launch-version.cjs <application-id> [--version 0] [--apply] | --all [--version 0] [--apply]');
  return {title, all, apply, version};
}

async function openNativeService(memory, name) {
  const state = await memory.request({command: 'root-native-info'});
  let service = state.services?.find(item => item.name === name);
  let openedHere = false;
  if (!service) {
    const sm = state.services?.find(item => item.name === 'pm:shell')?.sm;
    check(sm, 'The retained helper has no initialized SM client; start the ns2jb retained helper first');
    const lookup = await memory.request({command: 'root-native-sm-service', handle: sm, name});
    check(!lookup.transport && !lookup.result && lookup.moves?.length === 1,
      'The retained helper could not open ' + name + ': ' +
        (lookup.transport ? 'transport 0x' + Number(lookup.transport).toString(16) :
          'result 0x' + Number(lookup.result).toString(16)));
    service = {name, handle: lookup.moves[0]};
    openedHere = true;
  }
  return {service, openedHere};
}

async function openNativeNsManager(memory, nsService) {
  const opened = await memory.request({command: 'root-native-ipc',
    handle: nsService.handle, id: 7996, hex: ''});
  check(!opened.transport && !opened.result && opened.moves?.length === 1,
    'Could not open NS application-manager interface: ' +
      (opened.transport ? 'transport 0x' + Number(opened.transport).toString(16) :
        'result 0x' + Number(opened.result).toString(16)));
  return opened.moves[0];
}

async function pushNativeNsVersion(memory, manager, title, version) {
  const input = Buffer.alloc(24); // {u32 version; padding; u64 application_id; padding}
  input.writeUInt32LE(version, 0);
  input.writeBigUInt64LE(title, 8);
  const reply = await memory.request({command: 'root-native-ipc', handle: manager,
    id: 36, hex: input.toString('hex')});
  check(!reply.transport && !reply.result,
    'NS PushLaunchVersion failed: ' +
      (reply.transport ? 'transport 0x' + Number(reply.transport).toString(16) :
        'result 0x' + Number(reply.result).toString(16)));
}

async function pushAvmVersion(memory, avm, title, version) {
  const input = Buffer.alloc(16); // {u32 version; padding; u64 application_id;}
  input.writeUInt32LE(version, 0);
  input.writeBigUInt64LE(title, 8);
  const reply = await memory.request({command: 'root-native-ipc', handle: avm,
    id: 1000, hex: input.toString('hex')});
  check(!reply.transport && !reply.result,
    'AVM PushLaunchVersion failed: ' +
      (reply.transport ? 'transport 0x' + Number(reply.transport).toString(16) :
        'result 0x' + Number(reply.result).toString(16)));
}

async function listApplications(s, manager) {
  const pageSize = 64, recordSize = 0x18, limit = 8192;
  const address = s.base + 0x3000, size = pageSize * recordSize;
  check(address + size <= s.base + s.bytes, 'NS application-record page exceeds the owned allocation');
  const ids = [], seen = new Set();
  for (let offset = 0; offset < limit;) {
    const input = Buffer.alloc(4);
    input.writeInt32LE(offset, 0);
    await s.write(address, Buffer.alloc(size));
    const reply = await s.ipc('NS ListApplicationRecord offset ' + offset,
      manager, 0, input, {receive: [{address, size, mode: 0}]});
    check(!reply.result && reply.data.length >= 4,
      'NS ListApplicationRecord failed: 0x' + reply.result.toString(16));
    const count = reply.data.readInt32LE(0);
    check(count >= 0 && count <= pageSize, 'NS returned an invalid application-record count');
    if (!count) break;
    const bytes = await s.readOwned(address, count * recordSize);
    for (let i = 0; i < count; i++) {
      const id = bytes.readBigUInt64LE(i * recordSize);
      if (id && !seen.has(id.toString())) {
        seen.add(id.toString());
        ids.push(id);
      }
    }
    offset += count;
    check(offset <= limit, 'NS application-record list exceeded the safety limit');
    if (count < pageSize) break;
  }
  return ids;
}

function idText(id) {
  return '0x' + id.toString(16).padStart(16, '0');
}

function uncertainTransport(error) {
  return /transport failed|timed out|disconnected|connection closed|pending/i.test(error.message || '');
}

async function run(s, {title, all, apply, version}) {
  let memory, nativeNs, avm, manager;
  try {
    memory = await connect();
    let targets = all ? null : [title];
    if (apply || all) {
      nativeNs = await openNativeService(memory, 'ns:am2');
      manager = await openNativeNsManager(memory, nativeNs.service);
      s.handles.add(manager);
      if (all) targets = await listApplications(s, manager);
      check(targets.length > 0, 'NS returned no application records to process');
    }
    if (apply) {
      avm = await openNativeService(memory, 'avm');
    }
    const result = {session: live.session, all, apply, targetVersion: version,
      targetVersionText: version === 0 ? '1.0.0 (version code 0)' : null,
      total: targets.length, applications: []};
    let proofFile;
    if (apply) {
      proofFile = runPath('switch2', 'launch-version-reset-' + Date.now() + '.json');
      result.proofFile = proofFile;
    }
    const saveProof = proofFile ? () => fs.writeFile(proofFile,
      JSON.stringify(result, null, 2) + '\n', {mode: 0o600}) : async () => {};
    await saveProof();

    for (const id of targets) {
      const entry = {titleId: idText(id), status: apply ? 'pending' : 'dry-run'};
      result.applications.push(entry);
      await saveProof();
      if (!apply) continue;
      try {
        await pushAvmVersion(memory, avm.service.handle, id, version);
        entry.avmPushed = true;
        await saveProof();
        await pushNativeNsVersion(memory, manager, id, version);
        entry.nsPushed = true;
        entry.status = 'pushed';
      } catch (error) {
        entry.status = 'error';
        entry.error = error.message;
        await saveProof();
        if (uncertainTransport(error)) {
          error.proofFile = proofFile;
          throw error;
        }
      }
      await saveProof();
    }
    result.pushed = result.applications.filter(item => item.status === 'pushed').length;
    result.errors = result.applications.filter(item => item.status === 'error').length;
    await saveProof();
    return result;
  } finally {
    if (manager && s.handles.has(manager) && await s.ready()) await s.close(manager);
    if (avm?.openedHere && memory && (await s.ready()))
      await memory.request({command: 'root-native-close-handle', handle: avm.service.handle});
    if (nativeNs?.openedHere && memory && (await s.ready()))
      await memory.request({command: 'root-native-close-handle', handle: nativeNs.service.handle});
  }
}

async function main() {
  const options = parseArgs(process.argv.slice(2));
  const s = new Session('launch-version-reset-' + Date.now());
  try {
    await s.initialize();
    console.log(JSON.stringify(await run(s, options), null, 2));
  } finally {
    await s.cleanup();
  }
}

if (require.main === module) main().catch(error => {
  console.error(error.stack || error.message);
  if (error.proofFile) console.error('Partial update record: ' + error.proofFile);
  process.exitCode = 1;
});

module.exports = {parseArgs, run};
