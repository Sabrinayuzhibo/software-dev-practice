import assert from 'node:assert/strict'

/** Ordinary dialogs stay editable while the surrounding glass recovers. */
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
            const dialog = document.querySelector('dialog');
            return dialog?.open && document.querySelector('${input}') &&
                document.querySelector('.field-glass-source')?.dataset.glassState === 'active';
        })()`),
        'plain dialog and surrounding glass fields',
    )
    assert(
        await page.evaluate(`(() => {
        const dialog = document.querySelector('dialog');
        return getComputedStyle(dialog).backgroundColor === 'rgb(255, 255, 255)' &&
            !dialog.querySelector(':scope > canvas') &&
            !document.querySelector('.sheet-glass-source') &&
            !!dialog.querySelector('button canvas.glass-surface');
    })()`),
        'dialog is opaque and its buttons keep glass',
    )
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

    // Replacing the surrounding field context does not remount the form or
    // the toggle renderer. Native input stays usable during the fallback.
    await page.evaluate(`
        globalThis.previousFieldCanvas = document.querySelector('.field-glass-source');
        globalThis.previousAnimatedCanvas = document.querySelector('.control-glass-source');
        previousFieldCanvas.getContext('webgl').getExtension('WEBGL_lose_context').loseContext();
    `)
    await until(
        () =>
            page.evaluate(
                `document.querySelector('.glass-field')?.dataset.glassState === 'fallback'`,
            ),
        'field fallback',
    )
    assert.equal(
        await page.evaluate(
            `getComputedStyle(document.querySelector('dialog')).backgroundColor`,
        ),
        'rgb(255, 255, 255)',
        'dialog remains solid during surrounding recovery',
    )
    await page.call('Input.insertText', { text: '继续编辑' })
    await until(
        () =>
            page.evaluate(`(() => {
            const field = document.querySelector('.field-glass-source');
            return field !== previousFieldCanvas &&
                field?.dataset.glassState === 'active';
        })()`),
        'static glass recovery',
    )
    assert.equal(
        await page.evaluate(`document.querySelector('${input}').value`),
        '玻璃输入检查继续编辑',
        'native form state survives field context replacement',
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
        () => page.evaluate(`document.querySelector('dialog')?.open`),
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
        'PASS: solid dialog, native typing, glass controls and field recovery, preserved focus/form state, Escape and reopen',
    )
}
