import assert from 'node:assert/strict'
import { readFile } from 'node:fs/promises'
import { createRequire } from 'node:module'
const require = createRequire(
    new URL('../apps/backup-desktop/package.json', import.meta.url),
)
const { transpileModule, ModuleKind } = require('typescript')
const sourceCode = await readFile(
    new URL('../apps/backup-desktop/src/scan-tree.ts', import.meta.url),
    'utf8',
)
const { outputText } = transpileModule(sourceCode, {
    compilerOptions: { module: ModuleKind.ESNext },
})
const { buildScanTree, visibleScanRows, isScanDirectory } = await import(
    `data:text/javascript;base64,${Buffer.from(outputText).toString('base64')}`
)

const source = [
    {
        path: 'docs/deep/文.txt',
        type: 'file',
        size: 10,
        sha256: 'f'.repeat(64),
    },
    { path: 'root.txt', type: 'file', size: 0 },
    { path: 'docs', type: 'directory' },
    { path: 'docs/deep', type: 'directory' },
    { path: 'empty', type: 'directory' },
    { path: 'docs/alias', type: 'symlink', link_target: 'deep' },
    { path: '__proto__/file.txt', type: 'file', size: 3 },
]
const tree = buildScanTree(source)
assert.equal(tree.find((node) => node.path === 'docs').entry, source[2])
assert.equal(tree.at(-1).path, 'root.txt')
assert(tree.slice(0, -1).every(isScanDirectory))
assert.deepEqual(
    buildScanTree([
        { path: 'file10', type: 'file' },
        { path: 'file2', type: 'file' },
    ]).map((node) => node.path),
    ['file2', 'file10'],
)
const all = visibleScanRows(tree)
assert.equal(all.length, 8)
assert.equal(all.find(({ node }) => node.path === 'docs/deep/文.txt').depth, 2)
assert.equal(
    all.find(({ node }) => node.path === 'docs/deep/文.txt').node.entry.sha256,
    'f'.repeat(64),
)
assert.equal(
    isScanDirectory(all.find(({ node }) => node.path === 'docs/alias').node),
    false,
)
assert.equal(
    all.find(({ node }) => node.path === '__proto__').node.entry,
    undefined,
)
assert(
    !visibleScanRows(tree, new Set()).some(
        ({ node }) => node.path === 'docs/deep',
    ),
)
assert(
    visibleScanRows(tree, new Set(['docs'])).some(
        ({ node }) => node.path === 'docs/deep',
    ),
)
assert(
    !visibleScanRows(tree, new Set(['docs'])).some(
        ({ node }) => node.path === 'docs/deep/文.txt',
    ),
)
assert(
    visibleScanRows(tree, new Set(['docs', 'docs/deep'])).some(
        ({ node }) => node.path === 'docs/deep/文.txt',
    ),
)
assert.equal(visibleScanRows(tree, new Set()).length, tree.length)
assert.deepEqual(buildScanTree([]), [])
const deep =
    Array.from({ length: 1500 }, (_, index) => `d${index}`).join('/') + '/leaf'
assert.equal(
    visibleScanRows(buildScanTree([{ path: deep, type: 'file' }])).length,
    1501,
)
assert.deepEqual(
    source.map((entry) => entry.path),
    [
        'docs/deep/文.txt',
        'root.txt',
        'docs',
        'docs/deep',
        'empty',
        'docs/alias',
        '__proto__/file.txt',
    ],
)
console.log(
    'PASS: parent reconstruction, directories first, collapse, metadata, symlinks, special names and deep paths',
)
