import { spawn, type ChildProcessWithoutNullStreams } from 'node:child_process'
import { existsSync } from 'node:fs'
import { resolve } from 'node:path'
import { randomUUID } from 'node:crypto'

const maxReplyBytes = 2 * 1024 * 1024

interface AgentReply {
    id: string
    ok: boolean
    result?: unknown
    error?: { message?: string }
}

interface Pending {
    resolve: (value: unknown) => void
    reject: (reason: Error) => void
    timer: NodeJS.Timeout
}

export class AgentBridge {
    constructor(private readonly stateDirectory: string) {}
    private process?: ChildProcessWithoutNullStreams
    private buffer = ''
    private bufferBytes = 0
    private discarding = false
    private readonly pending = new Map<string, Pending>()
    private queue: Promise<unknown> = Promise.resolve()
    private stopped = false

    private start(): void {
        if (this.process) {
            return
        }
        const executable =
            process.env.BACKUP_AGENT_BIN ||
            resolve(__dirname, '../../../build/backup-agent')
        if (!existsSync(executable)) {
            throw new Error(
                `找不到 C++ Backup Agent：${executable}。请先运行 ./scripts/build.sh`,
            )
        }
        const child = spawn(executable, ['--state-dir', this.stateDirectory], {
            stdio: ['pipe', 'pipe', 'pipe'],
        })
        this.process = child
        this.buffer = ''
        this.bufferBytes = 0
        this.discarding = false
        child.stdout.setEncoding('utf8')
        child.stdout.on('data', (chunk: string) => this.receive(chunk))
        child.stderr.on('data', (chunk: Buffer) => {
            console.error(`Backup Agent: ${chunk.toString().trim()}`)
        })
        child.on('error', (error) => {
            if (this.process === child) {
                this.close(error)
            }
        })
        child.on('exit', (code) => {
            if (this.process === child) {
                this.close(
                    new Error(`Backup Agent 已退出（代码 ${code ?? '未知'}）`),
                )
            }
        })
    }

    private receive(chunk: string): void {
        let start = 0
        while (start < chunk.length) {
            const end = chunk.indexOf('\n', start)
            const part = chunk.slice(start, end < 0 ? undefined : end)
            start = end < 0 ? chunk.length : end + 1
            if (!this.discarding) {
                this.bufferBytes += Buffer.byteLength(part, 'utf8')
                if (this.bufferBytes > maxReplyBytes) {
                    // Qt emits id first in successful replies. Only reject that
                    // request; a late reply must not fail a newer queued request.
                    const prefix = this.buffer || part.slice(0, 256)
                    const id = /^\{\s*"id"\s*:\s*"([0-9a-f-]{36})"/.exec(
                        prefix,
                    )?.[1]
                    const request = id ? this.pending.get(id) : undefined
                    if (id && request) {
                        clearTimeout(request.timer)
                        this.pending.delete(id)
                        request.reject(new Error('查询结果过大，请分页读取'))
                    }
                    this.buffer = ''
                    this.discarding = true
                } else {
                    this.buffer += part
                }
            }
            if (end < 0) {
                continue
            }
            const line = this.buffer
            const discarded = this.discarding
            this.buffer = ''
            this.bufferBytes = 0
            this.discarding = false
            if (discarded) {
                continue
            }
            let response: AgentReply
            try {
                response = JSON.parse(line) as AgentReply
            } catch {
                const child = this.process
                this.close(new Error('Backup Agent 返回了无效 JSON'))
                child?.kill()
                return
            }
            const request = this.pending.get(response.id)
            if (!request) {
                continue
            }
            clearTimeout(request.timer)
            this.pending.delete(response.id)
            if (response.ok) {
                request.resolve(response.result)
            } else {
                request.reject(
                    new Error(
                        response.error?.message || 'Backup Agent 命令失败',
                    ),
                )
            }
        }
    }

    private close(error: Error): void {
        this.process = undefined
        for (const request of this.pending.values()) {
            clearTimeout(request.timer)
            request.reject(error)
        }
        this.pending.clear()
        this.buffer = ''
        this.bufferBytes = 0
        this.discarding = false
    }

    send<T>(action: string, payload: object, timeoutMs = 10000): Promise<T> {
        // The Agent has one worker. Serialize request acknowledgements so an
        // automatic health check cannot race a user's scan or configuration.
        const result = this.queue.then(() => {
            if (this.stopped) {
                throw new Error('应用正在关闭')
            }
            return this.request<T>(action, payload, timeoutMs)
        })
        this.queue = result.catch(() => {})
        return result
    }

    private request<T>(
        action: string,
        payload: object,
        timeoutMs: number,
    ): Promise<T> {
        this.start()
        const id = randomUUID()
        return new Promise<T>((resolve, reject) => {
            const timer = setTimeout(() => {
                this.pending.delete(id)
                reject(new Error('Backup Agent 响应超时'))
            }, timeoutMs)
            this.pending.set(id, {
                resolve: (value) => resolve(value as T),
                reject,
                timer,
            })
            this.process!.stdin.write(
                JSON.stringify({ id, action, payload }) + '\n',
                (error) => {
                    if (!error) {
                        return
                    }
                    const request = this.pending.get(id)
                    if (request) {
                        clearTimeout(request.timer)
                        this.pending.delete(id)
                        reject(error)
                    }
                },
            )
        })
    }

    stop(): void {
        this.stopped = true
        this.process?.stdin.end()
        this.process?.kill()
        this.close(new Error('应用正在关闭'))
    }
}
