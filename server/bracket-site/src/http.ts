// Small HTTP helpers the routes share.
import type { IncomingMessage, ServerResponse } from "node:http";

const MAX_BODY = 64 * 1024;

export type Json = Record<string, unknown>;

export class Failure extends Error {
  readonly status: number;
  readonly body: object;

  constructor(status: number, message: string, body: object = { error: message }) {
    super(message);
    this.status = status;
    this.body = body;
  }
}

export function fail(status: number, message: string): never {
  throw new Failure(status, message);
}

export function send(response: ServerResponse, status: number, body?: object): void {
  if (body === undefined) response.writeHead(status).end();
  else response.writeHead(status, { "content-type": "application/json" }).end(JSON.stringify(body));
}

export function text(value: unknown, pattern: RegExp, message: string): string {
  if (typeof value !== "string" || !pattern.test(value)) fail(400, message);
  return value;
}

export async function readBody(request: IncomingMessage): Promise<string> {
  let size = 0;
  const chunks: Buffer[] = [];
  for await (const chunk of request as AsyncIterable<Buffer>) {
    size += chunk.length;
    if (size > MAX_BODY) fail(413, "body too large");
    chunks.push(chunk);
  }
  return Buffer.concat(chunks).toString("utf8");
}

/** A JSON object body; an empty body is `{}`. */
export async function readJson(request: IncomingMessage): Promise<Json> {
  const raw = await readBody(request);
  if (!raw) return {};
  try {
    const body = JSON.parse(raw) as unknown;
    if (typeof body === "object" && body !== null && !Array.isArray(body)) return body as Json;
  } catch {
    // Answered below.
  }
  return fail(400, "the body is not a JSON object");
}

export type Handler = (request: IncomingMessage, params: string[], url: URL) => Promise<[number, object?]> | [number, object?];
export type Route = [method: string, path: RegExp, handler: Handler];
