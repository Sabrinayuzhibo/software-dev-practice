import assert from 'node:assert/strict'
import { setTimeout as delay } from 'node:timers/promises'
import { checkControlMotion } from './desktop_control_motion.mjs'

async function requireControls(page, until) {
    await until(
        () =>
            page.evaluate(`(() => {
        const source = document.querySelector('.control-glass-source');
        return source?.dataset.glassState === 'active' &&
            !source.getContext('webgl').isContextLost();
    })()`),
        'shared control renderer',
    )
}

export async function checkGlassControls({ page, click, until }) {
    await requireControls(page, until)
    await click('.primary-button')
    await until(
        () =>
            page.evaluate(`document.querySelectorAll(
        '.glass-switch[data-glass-state="active"]').length === 5`),
        'upstream switches',
    )
    const input = '[data-file-type="file"] input'
    await page.evaluate(`
        globalThis.controlDocumentMarker = 'preserved';
        globalThis.controlChanges = 0;
        document.querySelector('${input}').addEventListener('change', () => controlChanges++);
        globalThis.surfacePixels = () => {
            const canvas = document.querySelector('[data-file-type="file"] canvas');
            const pixels = canvas.getContext('2d').getImageData(0, 0, canvas.width, canvas.height).data;
            return Array.from(pixels).reduce((hash, value) => (hash * 31 + value) >>> 0, 0);
        };
    `)
    assert.equal(
        await page.evaluate(`document.querySelector('${input}').checked`),
        true,
    )
    await click(input)
    assert.equal(
        await page.evaluate(`document.querySelector('${input}').checked`),
        false,
    )
    await page.evaluate(`document.querySelector('${input}').focus()`)
    await page.call('Input.dispatchKeyEvent', {
        type: 'keyDown',
        key: ' ',
        code: 'Space',
        windowsVirtualKeyCode: 32,
    })
    await page.call('Input.dispatchKeyEvent', {
        type: 'keyUp',
        key: ' ',
        code: 'Space',
        windowsVirtualKeyCode: 32,
    })
    assert.equal(
        await page.evaluate(`document.querySelector('${input}').checked`),
        true,
    )
    assert.equal(await page.evaluate('controlChanges'), 2)
    await delay(1200)
    const idlePixels = await page.evaluate('surfacePixels()')
    const point = await page.evaluate(`(() => {
        const r = document.querySelector('${input}').getBoundingClientRect();
        return {x: r.x + 40, y: r.y + r.height / 2};
    })()`)
    await page.call('Input.dispatchMouseEvent', {
        type: 'mouseMoved',
        ...point,
    })
    await page.call('Input.dispatchMouseEvent', {
        type: 'mousePressed',
        button: 'left',
        buttons: 1,
        clickCount: 1,
        ...point,
    })
    await delay(300)
    assert.notEqual(
        await page.evaluate('surfacePixels()'),
        idlePixels,
        'pressed glass pixels change',
    )
    await checkControlMotion({ page, until, point })
    await page.call('Input.dispatchMouseEvent', {
        type: 'mouseMoved',
        buttons: 1,
        x: point.x - 36,
        y: point.y,
    })
    await page.call('Input.dispatchMouseEvent', {
        type: 'mouseReleased',
        button: 'left',
        buttons: 0,
        clickCount: 1,
        x: point.x - 36,
        y: point.y,
    })
    assert.equal(
        await page.evaluate(`document.querySelector('${input}').checked`),
        false,
        'drag commits once',
    )
    await click(input)
    assert.equal(
        await page.evaluate(`document.querySelector('${input}').checked`),
        true,
        'next click is not swallowed',
    )
    await requireControls(page, until)

    // Native fieldset disabling also blocks drag, keyboard and label activation.
    await page.evaluate(
        "document.querySelector('.file-type-picker').disabled = true",
    )
    await click(input)
    assert.equal(
        await page.evaluate(`document.querySelector('${input}').checked`),
        true,
    )
    await page.evaluate(
        "document.querySelector('.file-type-picker').disabled = false",
    )
    assert.equal(
        await page.evaluate("document.querySelector('.create-task').disabled"),
        true,
    )

    await page.evaluate(`
        globalThis.oldControlCanvas = document.querySelector('.control-glass-source');
        oldControlCanvas.getContext('webgl').getExtension('WEBGL_lose_context').loseContext();
    `)
    await until(
        () =>
            page.evaluate(
                "document.querySelector('.glass-switch').dataset.glassState === 'fallback'",
            ),
        'control context loss',
    )
    await click(input)
    assert.equal(
        await page.evaluate(`document.querySelector('${input}').checked`),
        false,
        'fallback still operates',
    )
    await until(
        () =>
            page.evaluate(
                "document.querySelector('.control-glass-source') !== oldControlCanvas",
            ),
        'control context replacement',
    )
    await requireControls(page, until)
    assert.equal(await page.evaluate('controlDocumentMarker'), 'preserved')
    assert.equal(
        await page.evaluate(`document.querySelector('${input}').checked`),
        false,
    )
    assert.equal(
        await page.evaluate(
            "document.querySelectorAll('.control-glass-source').length",
        ),
        1,
    )
    await click('.special-type-toggle')
    await until(
        () =>
            page.evaluate(
                "document.querySelectorAll('.glass-switch[data-glass-state=active]').length === 8",
            ),
        'expanded switches',
    )
    await click('[aria-label="关闭对话框"]')
    await until(
        () => page.evaluate("!document.querySelector('dialog')"),
        'control dialog closes',
    )
    console.log(
        'PASS: upstream button/switch surfaces, animation pixels, click/keyboard/drag, disabled fieldset, live fallback and context recovery preserve form state',
    )
}
