// The JSON-object guard the self-test tools share.
import type { Json } from "../src/index.ts";

export type Body = { [key: string]: Json };

export const isBody = (value: Json | undefined): value is Body => typeof value === "object" && value !== null && !Array.isArray(value);
