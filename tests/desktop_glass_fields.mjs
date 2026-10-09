import assert from 'node:assert/strict'

/** Exercise native typing and modal behavior while static glass is replaced. */
export async function checkGlassFields({ page, click, until }) {
    const typography = await page.evaluate(`(async () => {
        const loaded = await document.fonts.load(
            '13px "Backup Noto Sans SC"', '备份目录中文文件'
        );
        return {
            loaded: loaded.some(font => font.status === 'loaded'),
            family: getComputedStyle(document.documentElement).fontFamily,
        };
    })()`)
    assert(typography.loaded, 'bundled Chinese fallback font loads')
    assert(
        typography.family.indexOf('SF Pro SC') <
            typography.family.indexOf('PingFang SC'),
        'Apple simplified Chinese font order',
    )
    await page.evaluate(`(() => {
        globalThis.controlErrorQueries = 0;
        const original = WebGLRenderingContext.prototype.getError;
        WebGLRenderingContext.prototype.getError = function() {
            if (this.canvas.classList.contains('control-glass-source') ||
                this.canvas.classList.contains('field-glass-source') ||
                this.canvas.classList.contains('sheet-glass-source')) {
                controlErrorQueries++;
            }
            return original.call(this);
        };
    })()`)
    await click('.primary-button')
    const input = 'input[aria-label="任务名称"]'
    await until(
        () =>
            page.evaluate(`(() => {
            const field = document.querySelector('${input}')?.closest('.glass-field');
            return field?.dataset.glassState === 'active' &&
                document.querySelector('dialog')?.dataset.glassState === 'active';
        })()`),
        'glass fields and dialog',
    )
    const pixels = await page.evaluate(`(() => {
        const canvas = document.querySelector('dialog > canvas');
        const context = canvas.getContext('2d');
        const x = Math.floor(canvas.width / 2);
        const y = Math.floor(canvas.height / 2);
        return [
            [...context.getImageData(x, y, 1, 1).data],
            [...context.getImageData(0, 0, 1, 1).data],
        ];
    })()`)
    assert.equal(pixels[0][3], 255, 'dialog material is painted')
    assert.equal(pixels[1][3], 0, 'dialog keeps transparent rounded corners')
    assert.equal(
        await page.evaluate('controlErrorQueries'),
        0,
        'glass surfaces do not stall software rendering with getError',
    )
    await click(input)
    await page.call('Input.insertText', { text: '玻璃输入检查' })
    assert.equal(
        await page.evaluate(`document.querySelector('${input}').value`),
        '玻璃输入检查',
    )

    // Replacing either static context must not remount the form or the live
    // toggle renderer. Native input also stays usable during the fallback.
    await page.evaluate(`
        globalThis.previousFieldCanvas = document.querySelector('.field-glass-source');
        globalThis.previousSheetCanvas = document.querySelector('.sheet-glass-source');
        globalThis.previousAnimatedCanvas = document.querySelector('.control-glass-source');
        for (const canvas of [previousFieldCanvas, previousSheetCanvas]) {
            canvas.getContext('webgl').getExtension('WEBGL_lose_context').loseContext();
        }
    `)
    await until(
        () =>
            page.evaluate(
                `document.querySelector('dialog').dataset.glassState === 'fallback'`,
            ),
        'dialog fallback',
    )
    assert.equal(
        await page.evaluate(
            `getComputedStyle(document.querySelector('dialog')).backgroundColor`,
        ),
        'rgba(226, 240, 249, 0.87)',
        'fallback keeps a tinted glass appearance',
    )
    await page.call('Input.insertText', { text: '继续编辑' })
    await until(
        () =>
            page.evaluate(`(() => {
            const field = document.querySelector('.field-glass-source');
            const sheet = document.querySelector('.sheet-glass-source');
            return field !== previousFieldCanvas && sheet !== previousSheetCanvas &&
                field?.dataset.glassState === 'active' &&
                sheet?.dataset.glassState === 'active';
        })()`),
        'static glass recovery',
    )
    assert.equal(
        await page.evaluate(`document.querySelector('${input}').value`),
        '玻璃输入检查继续编辑',
        'native form state survives both context replacements',
    )
    assert.equal(
        await page.evaluate(
            `document.querySelector('.control-glass-source') === previousAnimatedCanvas`,
        ),
        true,
        'static recovery leaves animated controls intact',
    )
    assert.equal(
        await page.evaluate(
            `document.activeElement === document.querySelector('${input}')`,
        ),
        true,
        'input focus survives recovery',
    )
    await page.call('Input.dispatchKeyEvent', {
        type: 'keyDown',
        key: 'Escape',
        code: 'Escape',
        windowsVirtualKeyCode: 27,
    })
    await page.call('Input.dispatchKeyEvent', {
        type: 'keyUp',
        key: 'Escape',
        code: 'Escape',
        windowsVirtualKeyCode: 27,
    })
    await until(
        () =>
            page.evaluate(
                `!document.querySelector('dialog, .sheet-glass-source')`,
            ),
        'Escape closes modal and releases its renderer',
    )
    await click('.primary-button')
    await until(
        () =>
            page.evaluate(
                `document.querySelector('dialog')?.dataset.glassState === 'active'`,
            ),
        'reopened dialog paints',
    )
    assert.equal(await page.evaluate('controlErrorQueries'), 0)
    assert.equal(
        await page.evaluate(`document.querySelector('${input}').value`),
        '',
        'a new task has a fresh form',
    )
    await click('[aria-label="关闭对话框"]')
    console.log(
        'PASS: glass input typing, dialog pixels, independent context recovery, preserved focus/form state, Escape and reopen',
    )
}
