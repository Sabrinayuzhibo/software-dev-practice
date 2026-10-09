import assert from 'node:assert/strict'

/** Inspect the displayed mask on each painted frame, using trusted mouse input. */
export async function checkControlMotion({ page, until, point }) {
    await page.evaluate(`
        globalThis.controlPaints = [];
        globalThis.controlKnobCenter = null;
        globalThis.originalControlCopy = CanvasRenderingContext2D.prototype.drawImage;
        globalThis.originalControlMask = CanvasRenderingContext2D.prototype.roundRect;
        CanvasRenderingContext2D.prototype.drawImage = function(source, ...args) {
            if (source.classList?.contains('control-glass-source')) {
                controlPaints.push(this.canvas.closest('[data-file-type]')?.dataset.fileType ?? 'other');
            }
            return originalControlCopy.call(this, source, ...args);
        };
        CanvasRenderingContext2D.prototype.roundRect = function(x, y, w, h, ...args) {
            if (this.canvas.closest('[data-file-type="file"]')) {
                controlKnobCenter = x + w / 2;
            }
            return originalControlMask.call(this, x, y, w, h, ...args);
        };
    `)
    try {
        // The 20px knob travel must follow both directions on its next frame,
        // even while the original press/shape spring is still moving.
        for (const delta of [-20, -5, -15]) {
            await page.evaluate('controlKnobCenter = null')
            await page.call('Input.dispatchMouseEvent', {
                type: 'mouseMoved',
                buttons: 1,
                x: point.x + delta,
                y: point.y,
            })
            await until(
                () => page.evaluate('controlKnobCenter !== null'),
                'drag paints its next position',
            )
            assert.ok(
                Math.abs(
                    (await page.evaluate('controlKnobCenter')) - (54 + delta),
                ) < 1,
                'displayed knob follows pointer without a position spring',
            )
        }
        const copies = await page.evaluate('controlPaints')
        assert.ok(copies.length > 0)
        assert.deepEqual(
            [...new Set(copies)],
            ['file'],
            'drag does not redraw any unchanged controls',
        )
    } finally {
        await page.evaluate(`
            CanvasRenderingContext2D.prototype.drawImage = originalControlCopy;
            CanvasRenderingContext2D.prototype.roundRect = originalControlMask;
        `)
    }
}
