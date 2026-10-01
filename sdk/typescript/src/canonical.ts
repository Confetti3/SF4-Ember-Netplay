// Strict JSON input and RFC 8785 canonical output, matching the bridge's
// rules: no duplicate keys, integers only within the safe range, at most 16
// levels of nesting. JSON.parse silently keeps the last duplicate key, so
// signed objects are parsed here instead.

export const MAX_DEPTH = 16;

export type Json = null | boolean | number | string | Json[] | { [key: string]: Json };

export class StrictJsonError extends Error {}

/** Parses `text` under the strict profile. */
export function parseStrict(text: string): Json {
  let at = 0;
  const fail = (why: string): never => {
    throw new StrictJsonError(`${why} at offset ${at}`);
  };
  const space = () => {
    while (at < text.length && " \t\n\r".includes(text[at]!)) at++;
  };
  const value = (depth: number): Json => {
    if (depth > MAX_DEPTH) fail("nesting is too deep");
    space();
    const ch = text[at];
    if (ch === "{") {
      at++;
      const object: { [key: string]: Json } = {};
      const seen = new Set<string>();
      space();
      if (text[at] === "}") {
        at++;
        return object;
      }
      for (;;) {
        space();
        if (text[at] !== '"') fail("expected a key");
        const key = string();
        if (seen.has(key)) fail("duplicate object key");
        seen.add(key);
        space();
        if (text[at++] !== ":") fail("expected ':'");
        Object.defineProperty(object, key, { value: value(depth + 1), enumerable: true, writable: true, configurable: true });
        space();
        if (text[at] === ",") {
          at++;
          continue;
        }
        if (text[at++] !== "}") fail("expected ',' or '}'");
        return object;
      }
    }
    if (ch === "[") {
      at++;
      const items: Json[] = [];
      space();
      if (text[at] === "]") {
        at++;
        return items;
      }
      for (;;) {
        items.push(value(depth + 1));
        space();
        if (text[at] === ",") {
          at++;
          continue;
        }
        if (text[at++] !== "]") fail("expected ',' or ']'");
        return items;
      }
    }
    if (ch === '"') return string();
    for (const [word, result] of [["true", true], ["false", false], ["null", null]] as const) {
      if (text.startsWith(word, at)) {
        at += word.length;
        return result;
      }
    }
    const match = /^-?(0|[1-9][0-9]*)/.exec(text.slice(at));
    if (!match) return fail("unexpected character");
    at += match[0].length;
    if (at < text.length && ".eE".includes(text[at]!)) fail("numbers must be integers");
    if (match[0] === "-0") fail("numbers must be integers");
    const number = Number(match[0]);
    if (!Number.isSafeInteger(number)) fail("integer is outside the safe range");
    return number;
  };
  const string = (): string => {
    const start = at;
    at++;
    for (;;) {
      if (at >= text.length) fail("unterminated string");
      const ch = text[at]!;
      if (ch === '"') {
        at++;
        break;
      }
      if (ch === "\\") at += text[at + 1] === "u" ? 6 : 2;
      else if (ch.charCodeAt(0) < 0x20) fail("control character in string");
      else at++;
    }
    const decoded = JSON.parse(text.slice(start, at)) as string;
    // Lone surrogates cannot be canonicalized or signed.
    if (!decoded.isWellFormed()) fail("invalid unicode in string");
    return decoded;
  };
  const result = value(0);
  space();
  if (at !== text.length) fail("trailing data");
  return result;
}

/** RFC 8785 bytes of `value`, which must already satisfy the strict profile. */
export function canonicalize(value: Json): string {
  if (value === null || typeof value === "boolean") return JSON.stringify(value);
  if (typeof value === "number") {
    if (!Number.isSafeInteger(value)) throw new StrictJsonError("numbers must be safe integers");
    return Object.is(value, -0) ? "0" : String(value);
  }
  if (typeof value === "string") {
    if (!value.isWellFormed()) throw new StrictJsonError("invalid unicode in string");
    return JSON.stringify(value);
  }
  if (Array.isArray(value)) return `[${value.map(canonicalize).join(",")}]`;
  // JavaScript compares strings by UTF-16 code units, which is the order
  // RFC 8785 requires.
  const keys = Object.keys(value).sort();
  return `{${keys.map((key) => `${JSON.stringify(key)}:${canonicalize(value[key]!)}`).join(",")}}`;
}
