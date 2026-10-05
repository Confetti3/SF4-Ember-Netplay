//! Strict JSON input and RFC 8785 (JCS) canonical output.
//!
//! Input is rejected before it becomes a map when it has duplicate keys, a
//! fraction or exponent, an integer outside the IEEE 754 safe range, more
//! than `MAX_DEPTH` levels, or trailing data. Because only safe integers are
//! admitted, JCS number formatting reduces to plain decimal, and the output
//! below is a complete JCS serializer for every value this module accepts.
use std::{collections::BTreeMap, fmt};

use serde::{
    Serialize,
    de::{self, DeserializeOwned, DeserializeSeed, MapAccess, SeqAccess, Visitor},
};
use sha2::{Digest, Sha256};

use crate::{Error, Result, encoding::b64u};

pub const MAX_DEPTH: usize = 16;
pub const MAX_SAFE_INTEGER: i64 = 9_007_199_254_740_991;
/// Default bound for general API and event bodies (spec 25.1).
pub const MAX_BODY: usize = 64 * 1024;

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Value {
    Null,
    Bool(bool),
    Int(i64),
    String(String),
    Array(Vec<Value>),
    Object(BTreeMap<String, Value>),
}

/// Parses `bytes` under the strict profile, after checking the size bound.
pub fn parse(bytes: &[u8], max_len: usize) -> Result<Value> {
    if bytes.len() > max_len {
        return Err(Error::TooLarge);
    }
    let mut deserializer = serde_json::Deserializer::from_slice(bytes);
    let value = Seed { depth: 0 }
        .deserialize(&mut deserializer)
        .map_err(|error| Error::InvalidJson(error.to_string()))?;
    deserializer
        .end()
        .map_err(|error| Error::InvalidJson(error.to_string()))?;
    Ok(value)
}

/// Parses strictly, then decodes into `T`. `T` should use
/// `deny_unknown_fields` so unexpected members are rejected too.
pub fn parse_as<T: DeserializeOwned>(bytes: &[u8], max_len: usize) -> Result<T> {
    let value = parse(bytes, max_len)?;
    from_value(&value)
}

/// Converts a strict value into `T`.
pub fn from_value<T: DeserializeOwned>(value: &Value) -> Result<T> {
    serde_json::from_value(value.to_serde()).map_err(|error| Error::InvalidJson(error.to_string()))
}

/// Converts any serializable value into the strict model, rejecting floats
/// and unsafe integers.
pub fn to_value<T: Serialize + ?Sized>(value: &T) -> Result<Value> {
    let serde =
        serde_json::to_value(value).map_err(|error| Error::InvalidJson(error.to_string()))?;
    Value::from_serde(&serde, 0)
}

/// JCS bytes of a serializable value.
pub fn canonical<T: Serialize + ?Sized>(value: &T) -> Result<Vec<u8>> {
    Ok(to_value(value)?.canonical())
}

/// Unpadded base64url SHA-256 of the JCS bytes: the `request_digest` and
/// rules/roster digest form.
pub fn digest<T: Serialize + ?Sized>(value: &T) -> Result<String> {
    Ok(b64u(&Sha256::digest(canonical(value)?)))
}

impl Value {
    /// RFC 8785 canonical bytes.
    pub fn canonical(&self) -> Vec<u8> {
        let mut out = Vec::new();
        self.write(&mut out);
        out
    }

    pub fn get(&self, key: &str) -> Option<&Value> {
        match self {
            Self::Object(map) => map.get(key),
            _ => None,
        }
    }

    pub fn as_str(&self) -> Option<&str> {
        match self {
            Self::String(text) => Some(text),
            _ => None,
        }
    }

    fn write(&self, out: &mut Vec<u8>) {
        match self {
            Self::Null => out.extend_from_slice(b"null"),
            Self::Bool(true) => out.extend_from_slice(b"true"),
            Self::Bool(false) => out.extend_from_slice(b"false"),
            Self::Int(number) => out.extend_from_slice(number.to_string().as_bytes()),
            Self::String(text) => write_string(text, out),
            Self::Array(items) => {
                out.push(b'[');
                for (index, item) in items.iter().enumerate() {
                    if index > 0 {
                        out.push(b',');
                    }
                    item.write(out);
                }
                out.push(b']');
            }
            Self::Object(map) => {
                // JCS orders members by UTF-16 code units, which differs from
                // the map's UTF-8 order once keys leave the BMP.
                let mut entries: Vec<_> = map.iter().collect();
                entries.sort_by(|(a, _), (b, _)| a.encode_utf16().cmp(b.encode_utf16()));
                out.push(b'{');
                for (index, (key, item)) in entries.into_iter().enumerate() {
                    if index > 0 {
                        out.push(b',');
                    }
                    write_string(key, out);
                    out.push(b':');
                    item.write(out);
                }
                out.push(b'}');
            }
        }
    }

    fn to_serde(&self) -> serde_json::Value {
        match self {
            Self::Null => serde_json::Value::Null,
            Self::Bool(value) => serde_json::Value::Bool(*value),
            Self::Int(number) => serde_json::Value::from(*number),
            Self::String(text) => serde_json::Value::String(text.clone()),
            Self::Array(items) => {
                serde_json::Value::Array(items.iter().map(Self::to_serde).collect())
            }
            Self::Object(map) => serde_json::Value::Object(
                map.iter()
                    .map(|(key, item)| (key.clone(), item.to_serde()))
                    .collect(),
            ),
        }
    }

    fn from_serde(value: &serde_json::Value, depth: usize) -> Result<Self> {
        if depth > MAX_DEPTH {
            return Err(Error::InvalidJson("nesting is too deep".into()));
        }
        Ok(match value {
            serde_json::Value::Null => Self::Null,
            serde_json::Value::Bool(value) => Self::Bool(*value),
            serde_json::Value::Number(number) => Self::Int(
                number
                    .as_i64()
                    .filter(|value| (-MAX_SAFE_INTEGER..=MAX_SAFE_INTEGER).contains(value))
                    .ok_or_else(|| Error::InvalidJson("number is not a safe integer".into()))?,
            ),
            serde_json::Value::String(text) => Self::String(text.clone()),
            serde_json::Value::Array(items) => Self::Array(
                items
                    .iter()
                    .map(|item| Self::from_serde(item, depth + 1))
                    .collect::<Result<_>>()?,
            ),
            serde_json::Value::Object(map) => Self::Object(
                map.iter()
                    .map(|(key, item)| Ok((key.clone(), Self::from_serde(item, depth + 1)?)))
                    .collect::<Result<_>>()?,
            ),
        })
    }
}

fn write_string(text: &str, out: &mut Vec<u8>) {
    out.push(b'"');
    for ch in text.chars() {
        match ch {
            '"' => out.extend_from_slice(b"\\\""),
            '\\' => out.extend_from_slice(b"\\\\"),
            '\u{08}' => out.extend_from_slice(b"\\b"),
            '\u{0c}' => out.extend_from_slice(b"\\f"),
            '\n' => out.extend_from_slice(b"\\n"),
            '\r' => out.extend_from_slice(b"\\r"),
            '\t' => out.extend_from_slice(b"\\t"),
            ch if u32::from(ch) < 0x20 => {
                out.extend_from_slice(format!("\\u{:04x}", u32::from(ch)).as_bytes());
            }
            ch => {
                let mut buffer = [0; 4];
                out.extend_from_slice(ch.encode_utf8(&mut buffer).as_bytes());
            }
        }
    }
    out.push(b'"');
}

struct Seed {
    depth: usize,
}

impl<'de> DeserializeSeed<'de> for Seed {
    type Value = Value;

    fn deserialize<D: de::Deserializer<'de>>(
        self,
        deserializer: D,
    ) -> std::result::Result<Value, D::Error> {
        if self.depth > MAX_DEPTH {
            return Err(de::Error::custom("nesting is too deep"));
        }
        deserializer.deserialize_any(self)
    }
}

impl<'de> Visitor<'de> for Seed {
    type Value = Value;

    fn expecting(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str("strict JSON")
    }

    fn visit_unit<E: de::Error>(self) -> std::result::Result<Value, E> {
        Ok(Value::Null)
    }

    fn visit_bool<E: de::Error>(self, value: bool) -> std::result::Result<Value, E> {
        Ok(Value::Bool(value))
    }

    fn visit_i64<E: de::Error>(self, value: i64) -> std::result::Result<Value, E> {
        if !(-MAX_SAFE_INTEGER..=MAX_SAFE_INTEGER).contains(&value) {
            return Err(E::custom("integer is outside the safe range"));
        }
        Ok(Value::Int(value))
    }

    fn visit_u64<E: de::Error>(self, value: u64) -> std::result::Result<Value, E> {
        if value > MAX_SAFE_INTEGER as u64 {
            return Err(E::custom("integer is outside the safe range"));
        }
        Ok(Value::Int(value as i64))
    }

    fn visit_f64<E: de::Error>(self, _: f64) -> std::result::Result<Value, E> {
        Err(E::custom("numbers must be integers"))
    }

    fn visit_str<E: de::Error>(self, value: &str) -> std::result::Result<Value, E> {
        Ok(Value::String(value.to_owned()))
    }

    fn visit_string<E: de::Error>(self, value: String) -> std::result::Result<Value, E> {
        Ok(Value::String(value))
    }

    fn visit_seq<A: SeqAccess<'de>>(self, mut seq: A) -> std::result::Result<Value, A::Error> {
        let mut items = Vec::new();
        while let Some(item) = seq.next_element_seed(Seed {
            depth: self.depth + 1,
        })? {
            items.push(item);
        }
        Ok(Value::Array(items))
    }

    fn visit_map<A: MapAccess<'de>>(self, mut map: A) -> std::result::Result<Value, A::Error> {
        let mut members = BTreeMap::new();
        while let Some(key) = map.next_key::<String>()? {
            if members.contains_key(&key) {
                return Err(de::Error::custom("duplicate object key"));
            }
            let item = map.next_value_seed(Seed {
                depth: self.depth + 1,
            })?;
            members.insert(key, item);
        }
        Ok(Value::Object(members))
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn jcs(text: &str) -> String {
        String::from_utf8(parse(text.as_bytes(), MAX_BODY).unwrap().canonical()).unwrap()
    }

    #[test]
    fn rejects_non_strict_input() {
        for bad in [
            r#"{"a":1,"a":2}"#,
            r#"{"a":{"b":1,"b":1}}"#,
            r#"{"n":1.5}"#,
            r#"{"n":1.0}"#,
            r#"{"n":1e3}"#,
            r#"{"n":9007199254740992}"#,
            r#"{"n":-9007199254740992}"#,
            r#"{"n":-9223372036854775808}"#,
            r#"{"n":18446744073709551616}"#,
            r#"{"a":1} x"#,
            r#"{"a":"\ud800"}"#,
            r#"{"a":NaN}"#,
            "",
        ] {
            assert!(parse(bad.as_bytes(), MAX_BODY).is_err(), "{bad}");
        }
        assert!(parse(&[b'"', 0xff, b'"'], MAX_BODY).is_err());
        assert_eq!(parse(b"[1]", 2), Err(Error::TooLarge));
    }

    #[test]
    fn bounds_nesting() {
        let ok = format!("{}0{}", "[".repeat(MAX_DEPTH), "]".repeat(MAX_DEPTH));
        assert!(parse(ok.as_bytes(), MAX_BODY).is_ok());
        let deep = format!(
            "{}0{}",
            "[".repeat(MAX_DEPTH + 1),
            "]".repeat(MAX_DEPTH + 1)
        );
        assert!(parse(deep.as_bytes(), MAX_BODY).is_err());
    }

    #[test]
    fn rfc8785_orders_keys_by_utf16() {
        // RFC 8785 section 3.2.3 sorting example.
        let input = "{\"\\u20ac\":\"Euro Sign\",\"\\r\":\"Carriage Return\",\"\\ufb33\":\"Hebrew Letter Dalet With Dagesh\",\"1\":\"One\",\"\\ud83d\\ude00\":\"Emoji: Grinning Face\",\"\\u0080\":\"Control\",\"\\u00f6\":\"Latin Small Letter O With Diaeresis\"}";
        let output = jcs(input);
        let order: Vec<&str> = [
            "\\r",
            "1",
            "\u{80}",
            "\u{f6}",
            "\u{20ac}",
            "\u{1f600}",
            "\u{fb33}",
        ]
        .to_vec();
        let mut last = 0;
        for key in order {
            let at = output.find(&format!("\"{key}\":")).unwrap();
            assert!(at >= last, "{key}");
            last = at;
        }
    }

    #[test]
    fn rfc8785_string_escapes() {
        assert_eq!(
            jcs(r#"{"s":"\u0000\u001f\b\f\n\r\t\"\\/\u007f\u00e9\u2028"}"#),
            "{\"s\":\"\\u0000\\u001f\\b\\f\\n\\r\\t\\\"\\\\/\u{7f}\u{e9}\u{2028}\"}"
        );
        assert_eq!(
            jcs(r#"{"b":[true,false,null,0,-5,9007199254740991]}"#),
            r#"{"b":[true,false,null,0,-5,9007199254740991]}"#
        );
        // serde_json reads -0 as a float, so the strict profile refuses it.
        assert!(parse(b"-0", MAX_BODY).is_err());
    }

    #[test]
    fn typed_values_canonicalize() {
        #[derive(Serialize)]
        struct Sample {
            zeta: u8,
            alpha: &'static str,
        }
        assert_eq!(
            canonical(&Sample {
                zeta: 1,
                alpha: "x"
            })
            .unwrap(),
            br#"{"alpha":"x","zeta":1}"#
        );
        assert!(to_value(&1.5f64).is_err());
        assert!(to_value(&u64::MAX).is_err());
    }
}
