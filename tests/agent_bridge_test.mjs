import assert from 'node:assert/strict'
import { mkdtemp, writeFile, rm } from 'node:fs/promises'
import { tmpdir } from 'node:os'
import { join } from 'node:path'
import { setTimeout as delay } from 'node:timers/promises'
import { AgentBridge } from '../apps/backup-desktop/dist-electron/agent-bridge.js'

const directory = await mkdtemp(join(tmpdir(), 'backup-bridge-test-'))
const executable = join(directory, 'agent.mjs')
// Real child-process pipes exercise framing, late replies and queued requests.
await writeFile(
    executable,
    String.raw`#!/usr/bin/env node
import { createInterface } from 'node:readline';
import { setTimeout as delay } from 'node:timers/promises';
let ticks = 0;
const timer = setInterval(() => ticks++, 5);
const reply = (id, result) => JSON.stringify({ id, ok: true, result }) + '\n';
const oversized = id => reply(id, '\u4e2d'.repeat(800000));
let pendingLate = null;
let queue = Promise.resolve();
createInterface({ input: process.stdin }).on('line', line => {
    queue = queue.then(async () => {
    const command = JSON.parse(line);
    if (command.action === 'oversized') {
        const message = oversized(command.id);
        for (let offset = 0; offset < message.length; offset += 40000) {
            process.stdout.write(message.slice(offset, offset + 40000));
            await delay(2);
        }
    } else if (command.action === 'late') {
        pendingLate = command.id;
    } else if (command.action === 'status') {
        if (pendingLate) {
            process.stdout.write(oversized(pendingLate));
            pendingLate = null;
        }
        process.stdout.write(reply(command.id, { pid: process.pid, ticks }));
    } else if (command.action === 'coalesced') {
        // Two individually valid replies together exceed the receive limit.
        process.stdout.write(reply('unmatched', 'x'.repeat(1200000)) +
            reply(command.id, 'y'.repeat(1200000)));
    }
    });
}).on('close', () => { clearInterval(timer); process.exit(0); });
`,
    { mode: 0o700 },
)

const previous = process.env.BACKUP_AGENT_BIN
process.env.BACKUP_AGENT_BIN = executable
const bridge = new AgentBridge(directory)
try {
    const first = await bridge.send('status', {})
    await assert.rejects(bridge.send('oversized', {}), /查询结果过大/)
    const second = await bridge.send('status', {})
    assert.equal(
        second.pid,
        first.pid,
        'oversized response must not restart the Agent',
    )
    assert(second.ticks > first.ticks, 'background work must continue')
    assert.equal((await bridge.send('coalesced', {})).length, 1200000)
    await assert.rejects(bridge.send('late', {}, 30), /响应超时/)
    const afterLate = await bridge.send('status', {})
    assert.equal(
        afterLate.pid,
        first.pid,
        'late oversized reply must not reject a newer request',
    )
    console.log(
        'PASS: oversized UTF-8 reply fails only its query; Agent, background work, framing and subsequent requests survive',
    )
} finally {
    bridge.stop()
    if (previous === undefined) {
        delete process.env.BACKUP_AGENT_BIN
    } else {
        process.env.BACKUP_AGENT_BIN = previous
    }
    await delay(100)
    await rm(directory, { recursive: true, force: true })
}
