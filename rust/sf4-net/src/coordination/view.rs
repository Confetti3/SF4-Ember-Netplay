//! The parts of a committed native `SessionProposal` envelope the helper
//! reads, taken from the checkpoint text in one pass without building the
//! rest. The checkpoint can weigh hundreds of kilobytes, nearly all of it
//! the effect journal and room state the helper never looks at, and three
//! readers used to build a whole `Value` of it each.
//!
//! The view is a `Value` with only these paths, each kept whole:
//!
//! - `checkpoint.members`
//! - `checkpoint.room.snapshot.members`
//! - `checkpoint.room.snapshot.tables`
//! - `checkpoint.room.snapshot.tournament`
//!
//! so the readers run their `Value` code on it unchanged, and it answers
//! every one of their questions as the whole envelope would. It accepts
//! exactly the texts a `Value` parse accepts: the skipped parts go through
//! the same parser paths (strings are decoded, numbers are range-checked,
//! nesting counts toward the same depth limit), not a raw skip. A repeated
//! key keeps its last value, as a `Value` object does. An object along the
//! path that is something else in the text is left out of the view, which
//! the readers cannot tell from a wrong type there.

use std::fmt;

use serde::{
    Deserialize as _,
    de::{DeserializeSeed, Deserializer, MapAccess, SeqAccess, Visitor},
};
use serde_json::{Map, Value};

/// Which members of an object the view keeps.
enum Shape {
    /// The whole value.
    Whole,
    /// An object: these members, each in its own shape; nothing else.
    Object(&'static [(&'static str, Shape)]),
}

const ENVELOPE: Shape = Shape::Object(&[(
    "checkpoint",
    Shape::Object(&[
        ("members", Shape::Whole),
        (
            "room",
            Shape::Object(&[(
                "snapshot",
                Shape::Object(&[
                    ("members", Shape::Whole),
                    ("tables", Shape::Whole),
                    ("tournament", Shape::Whole),
                ]),
            )]),
        ),
    ]),
)]);

/// The view of `checkpoint`, `None` when it is not JSON.
pub fn envelope_view(checkpoint: &str) -> Option<Value> {
    let mut parser = serde_json::Deserializer::from_str(checkpoint);
    let view = ENVELOPE.deserialize(&mut parser).ok()?;
    parser.end().ok()?;
    // An envelope that is not an object has nothing on the paths.
    Some(view.unwrap_or_else(|| Value::Object(Map::new())))
}

/// The view of an envelope that is already a `Value`, as the tests compare.
#[cfg(test)]
pub fn value_view(envelope: &Value) -> Value {
    ENVELOPE
        .deserialize(envelope)
        .ok()
        .flatten()
        .unwrap_or_else(|| Value::Object(Map::new()))
}

impl<'de> DeserializeSeed<'de> for &Shape {
    /// `None` for an object shape the text holds something else in.
    type Value = Option<Value>;

    fn deserialize<D: Deserializer<'de>>(self, deserializer: D) -> Result<Self::Value, D::Error> {
        match self {
            Shape::Whole => Value::deserialize(deserializer).map(Some),
            Shape::Object(members) => deserializer.deserialize_any(ObjectVisitor(members)),
        }
    }
}

struct ObjectVisitor(&'static [(&'static str, Shape)]);

impl<'de> Visitor<'de> for ObjectVisitor {
    type Value = Option<Value>;

    fn expecting(&self, formatter: &mut fmt::Formatter) -> fmt::Result {
        formatter.write_str("any JSON value")
    }

    fn visit_map<A: MapAccess<'de>>(self, mut map: A) -> Result<Self::Value, A::Error> {
        let mut kept = Map::new();
        while let Some(index) = map.next_key_seed(KeySeed(self.0))? {
            match index {
                Some(index) => {
                    let (name, shape) = &self.0[index];
                    // The last of a repeated key wins, as in a `Value`.
                    match map.next_value_seed(shape)? {
                        Some(value) => kept.insert((*name).to_owned(), value),
                        None => kept.remove(*name),
                    };
                }
                None => {
                    map.next_value::<Checked>()?;
                }
            }
        }
        Ok(Some(Value::Object(kept)))
    }

    fn visit_seq<A: SeqAccess<'de>>(self, seq: A) -> Result<Self::Value, A::Error> {
        CheckedVisitor.visit_seq(seq).map(|_| None)
    }

    fn visit_bool<E>(self, _: bool) -> Result<Self::Value, E> {
        Ok(None)
    }

    fn visit_i64<E>(self, _: i64) -> Result<Self::Value, E> {
        Ok(None)
    }

    fn visit_u64<E>(self, _: u64) -> Result<Self::Value, E> {
        Ok(None)
    }

    fn visit_f64<E>(self, _: f64) -> Result<Self::Value, E> {
        Ok(None)
    }

    fn visit_str<E>(self, _: &str) -> Result<Self::Value, E> {
        Ok(None)
    }

    fn visit_unit<E>(self) -> Result<Self::Value, E> {
        Ok(None)
    }

    fn visit_none<E>(self) -> Result<Self::Value, E> {
        Ok(None)
    }

    fn visit_some<D: Deserializer<'de>>(self, deserializer: D) -> Result<Self::Value, D::Error> {
        deserializer.deserialize_any(self)
    }
}

/// A member name: which kept member it is, if any. It is decoded as a
/// `Value` key is, escapes included, and compared without allocating.
struct KeySeed(&'static [(&'static str, Shape)]);

impl<'de> DeserializeSeed<'de> for KeySeed {
    type Value = Option<usize>;

    fn deserialize<D: Deserializer<'de>>(self, deserializer: D) -> Result<Self::Value, D::Error> {
        deserializer.deserialize_str(self)
    }
}

impl<'de> Visitor<'de> for KeySeed {
    type Value = Option<usize>;

    fn expecting(&self, formatter: &mut fmt::Formatter) -> fmt::Result {
        formatter.write_str("a member name")
    }

    fn visit_str<E>(self, key: &str) -> Result<Self::Value, E> {
        Ok(self.0.iter().position(|(name, _)| *name == key))
    }
}

/// A value read and dropped, with every check a `Value` parse makes:
/// `IgnoredAny` would skip strings and numbers unchecked and without the
/// depth limit, and so accept texts a `Value` parse refuses.
struct Checked;

impl<'de> serde::Deserialize<'de> for Checked {
    fn deserialize<D: Deserializer<'de>>(deserializer: D) -> Result<Self, D::Error> {
        deserializer.deserialize_any(CheckedVisitor)
    }
}

struct CheckedVisitor;

impl<'de> Visitor<'de> for CheckedVisitor {
    type Value = Checked;

    fn expecting(&self, formatter: &mut fmt::Formatter) -> fmt::Result {
        formatter.write_str("any JSON value")
    }

    fn visit_bool<E>(self, _: bool) -> Result<Checked, E> {
        Ok(Checked)
    }

    fn visit_i64<E>(self, _: i64) -> Result<Checked, E> {
        Ok(Checked)
    }

    fn visit_u64<E>(self, _: u64) -> Result<Checked, E> {
        Ok(Checked)
    }

    fn visit_f64<E>(self, _: f64) -> Result<Checked, E> {
        Ok(Checked)
    }

    fn visit_str<E>(self, _: &str) -> Result<Checked, E> {
        Ok(Checked)
    }

    fn visit_unit<E>(self) -> Result<Checked, E> {
        Ok(Checked)
    }

    fn visit_none<E>(self) -> Result<Checked, E> {
        Ok(Checked)
    }

    fn visit_some<D: Deserializer<'de>>(self, deserializer: D) -> Result<Checked, D::Error> {
        Checked::deserialize(deserializer)
    }

    fn visit_seq<A: SeqAccess<'de>>(self, mut seq: A) -> Result<Checked, A::Error> {
        while seq.next_element::<Checked>()?.is_some() {}
        Ok(Checked)
    }

    fn visit_map<A: MapAccess<'de>>(self, mut map: A) -> Result<Checked, A::Error> {
        while map.next_key::<CheckedKey>()?.is_some() {
            map.next_value::<Checked>()?;
        }
        Ok(Checked)
    }
}

/// A member name read and dropped, decoded as a `Value` key is.
struct CheckedKey;

impl<'de> serde::Deserialize<'de> for CheckedKey {
    fn deserialize<D: Deserializer<'de>>(deserializer: D) -> Result<Self, D::Error> {
        deserializer.deserialize_str(CheckedKeyVisitor)
    }
}

struct CheckedKeyVisitor;

impl<'de> Visitor<'de> for CheckedKeyVisitor {
    type Value = CheckedKey;

    fn expecting(&self, formatter: &mut fmt::Formatter) -> fmt::Result {
        formatter.write_str("a member name")
    }

    fn visit_str<E>(self, _: &str) -> Result<CheckedKey, E> {
        Ok(CheckedKey)
    }
}

#[cfg(test)]
mod tests;
