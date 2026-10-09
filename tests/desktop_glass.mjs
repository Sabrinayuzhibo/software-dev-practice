import assert from 'node:assert/strict'
import { setTimeout as delay } from 'node:timers/promises'

export async function requireGlass(page, until) {
    await until(
        () =>
            page.evaluate(`(() => {
                const canvas = document.querySelector('.sidebar-glass');
                const gl = canvas?.getContext('webgl');
                return canvas?.dataset.glassState === 'active' &&
                    gl && !gl.isContextLost();
            })()`),
        'software WebGL sidebar',
    )
}

export async function checkGlass({ page, main, click, until }) {
    await requireGlass(page, until)
    const driver = await page.evaluate(`(() => {
        const canvas = document.querySelector('.sidebar-glass');
        const gl = canvas.getContext('webgl');
        const info = gl.getExtension('WEBGL_debug_renderer_info');
        return info && gl.getParameter(info.UNMASKED_RENDERER_WEBGL);
    })()`)
    assert.match(driver, /SwiftShader/i)
    assert.equal(
        await main.evaluate(
            "testElectron.app.commandLine.getSwitchValue('use-angle')",
        ),
        'swiftshader',
    )

    await page.call('Page.enable')
    const injection = await page.call('Page.addScriptToEvaluateOnNewDocument', {
        source: `
            const original = HTMLCanvasElement.prototype.getContext;
            globalThis.glassCreationFailures = 0;
            HTMLCanvasElement.prototype.getContext = function(type, ...args) {
                if (type === 'webgl' && this.classList.contains('sidebar-glass') &&
                    glassCreationFailures < 2) {
                    glassCreationFailures++;
                    return null;
                }
                return original.call(this, type, ...args);
            };
        `,
    })
    try {
        await page.call('Page.reload')
        await until(
            () => page.evaluate('globalThis.glassCreationFailures === 2'),
            'transient WebGL creation failures',
        )
        await requireGlass(page, until)
    } finally {
        await page.call('Page.removeScriptToEvaluateOnNewDocument', injection)
    }
    await page.call('Page.reload')
    await until(
        () => page.evaluate('typeof glassCreationFailures === "undefined"'),
        'fresh page after removing context failure injection',
    )
    await requireGlass(page, until)
    await page.call('Emulation.setEmulatedMedia', {
        features: [{ name: 'prefers-reduced-motion', value: 'no-preference' }],
    })
    await main.evaluate(
        'testWindow.restore(); testWindow.show(); testWindow.focus();',
    )

    // Read pixels immediately after a draw, before Chromium discards the
    // default framebuffer. Production keeps preserveDrawingBuffer disabled.
    await page.evaluate(`
        globalThis.glassFrames = 0;
        globalThis.glassPixels = [];
        globalThis.glassDocumentMarker = 'preserved';
        const originalDraw = WebGLRenderingContext.prototype.drawArrays;
        WebGLRenderingContext.prototype.drawArrays = function(...args) {
            originalDraw.apply(this, args);
            if (!this.canvas.classList.contains('sidebar-glass')) return;
            if (this.getParameter(this.FRAMEBUFFER_BINDING) !== null) return;
            glassFrames++;
            const outer = this.canvas.getBoundingClientRect();
            const selected = document.querySelector('.nav-item.active')
                .getBoundingClientRect();
            const x = Math.floor(this.canvas.width / 2);
            const y = Math.floor(this.canvas.height *
                (1 - (selected.top + selected.height / 2 - outer.top) / outer.height));
            const pixels = new Uint8Array(8);
            this.readPixels(x, y, 1, 1, this.RGBA, this.UNSIGNED_BYTE,
                pixels.subarray(0, 4));
            this.readPixels(x, 10, 1, 1, this.RGBA, this.UNSIGNED_BYTE,
                pixels.subarray(4, 8));
            glassPixels = Array.from(pixels);
        };
    `)
    const pressPoint = await page.evaluate(`(() => {
        const rect = document.querySelector('[data-tab="versions"]')
            .getBoundingClientRect();
        return { x: rect.x + rect.width / 2, y: rect.y + rect.height / 2 };
    })()`)
    await page.call('Input.dispatchMouseEvent', {
        type: 'mouseMoved',
        ...pressPoint,
    })
    await page.call('Input.dispatchMouseEvent', {
        type: 'mousePressed',
        button: 'left',
        buttons: 1,
        clickCount: 1,
        ...pressPoint,
    })
    await until(
        () => page.evaluate('glassFrames > 1'),
        'pressed sidebar renders',
        8000,
    )
    await page.call('Input.dispatchMouseEvent', {
        type: 'mouseReleased',
        button: 'left',
        buttons: 0,
        clickCount: 1,
        ...pressPoint,
    })
    await until(
        async () => {
            const frames = await page.evaluate('glassFrames')
            await delay(400)
            return frames > 1 && frames === (await page.evaluate('glassFrames'))
        },
        'upstream spring settles and stops rendering',
        8000,
    ).catch(async (error) => {
        console.error(
            'GLASS ANIMATION',
            await page.evaluate(`({
            frames: glassFrames, pixels: glassPixels,
            selected: document.querySelector('.nav-item.active')?.dataset.tab,
                state: document.querySelector('.sidebar-glass')?.dataset.glassState,
            reduced: matchMedia('(prefers-reduced-motion: reduce)').matches
        })`),
        )
        throw error
    })
    const rendered = await page.evaluate(
        '({ frames: glassFrames, pixels: glassPixels })',
    )
    assert.ok(
        rendered.frames > 1,
        'upstream press spring should animate: ' + JSON.stringify(rendered),
    )
    assert.equal(rendered.pixels.length, 8)
    assert.ok(rendered.pixels.slice(0, 3).every((channel) => channel > 0))
    assert.notDeepEqual(
        rendered.pixels.slice(0, 3),
        rendered.pixels.slice(4, 7),
    )
    await delay(400)
    assert.equal(
        await page.evaluate('glassFrames'),
        rendered.frames,
        'idle sidebar should not continuously render on the CPU',
    )

    await page.evaluate(`
        globalThis.lostGlassCanvas = document.querySelector('.sidebar-glass');
        globalThis.glassLoss = lostGlassCanvas.getContext('webgl')
            .getExtension('WEBGL_lose_context');
        glassLoss.loseContext();
    `)
    await until(
        () =>
            page.evaluate("lostGlassCanvas.dataset.glassState === 'fallback'"),
        'context loss is handled',
    )
    await page.evaluate('glassLoss.restoreContext()')
    await requireGlass(page, until)
    assert.equal(
        await page.evaluate(
            "document.querySelector('.sidebar-glass') === lostGlassCanvas",
        ),
        true,
        'native context restoration should reuse the canvas',
    )

    await page.evaluate('glassLoss.loseContext()')
    await until(
        () =>
            page.evaluate(
                "document.querySelector('.sidebar-glass') !== lostGlassCanvas",
            ),
        'unrestored context triggers canvas replacement',
    )
    await requireGlass(page, until)
    assert.equal(await page.evaluate('glassDocumentMarker'), 'preserved')
    assert.equal(
        await page.evaluate(
            "document.querySelector('.nav-item.active').dataset.tab",
        ),
        'versions',
    )
    assert.ok(
        await page.evaluate(
            'glassPixels.slice(0, 3).every(value => value > 0)',
        ),
        'replacement context should draw visible pixels',
    )
    await click('[data-tab="tasks"]')
    await delay(350)
    console.log(
        'PASS: upstream glass with SwiftShader, transient initialization retry, nonblank animated screen pixels, idle rendering stop, native context restoration and canvas replacement without page reload',
    )
}
