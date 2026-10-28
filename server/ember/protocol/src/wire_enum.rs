//! One table per wire enum: each variant is spelled once, with its wire
//! name, and the macro writes the variant list (`ALL`), `as_str` and
//! `parse`. With `serde` it also gives each variant that name for
//! serde, so the derived `Serialize` and `Deserialize` agree with `as_str`
//! and an unknown name still fails to deserialize.

macro_rules! wire_enum {
    (
        serde
        $(#[$meta:meta])*
        $vis:vis enum $name:ident {
            $( $(#[$vmeta:meta])* $variant:ident = $text:literal, )+
        }
    ) => {
        $(#[$meta])*
        $vis enum $name {
            $( $(#[$vmeta])* #[serde(rename = $text)] $variant, )+
        }
        $crate::wire_enum::wire_enum!(@table $name { $( $variant = $text, )+ });
    };
    (
        plain
        $(#[$meta:meta])*
        $vis:vis enum $name:ident {
            $( $(#[$vmeta:meta])* $variant:ident = $text:literal, )+
        }
    ) => {
        $(#[$meta])*
        $vis enum $name {
            $( $(#[$vmeta])* $variant, )+
        }
        $crate::wire_enum::wire_enum!(@table $name { $( $variant = $text, )+ });
    };
    (@table $name:ident { $( $variant:ident = $text:literal, )+ }) => {
        impl $name {
            /// Every variant, in declaration order.
            pub const ALL: [$name; [$($text),+].len()] = [$( $name::$variant ),+];

            /// The variant's wire name.
            pub fn as_str(self) -> &'static str {
                match self {
                    $( $name::$variant => $text, )+
                }
            }

            /// The variant with this wire name, if there is one.
            pub fn parse(text: &str) -> Option<Self> {
                match text {
                    $( $text => Some($name::$variant), )+
                    _ => None,
                }
            }
        }
    };
}

pub(crate) use wire_enum;

#[cfg(test)]
mod tests {
    use std::fmt::Debug;

    use serde::{Serialize, de::DeserializeOwned};

    use crate::{
        api::ErrorCode,
        challenge::{Action, Method},
        event::Kind,
        lobby::Rotation,
        matches::{DeliveryState, MatchState},
        tournament::{Format, Side, Status},
    };

    /// Each name round trips through serde and `parse`, and no name repeats.
    fn names_agree<T>(all: &[T], as_str: fn(T) -> &'static str, parse: fn(&str) -> Option<T>)
    where
        T: Copy + Debug + PartialEq + Serialize + DeserializeOwned,
    {
        let mut seen = std::collections::BTreeSet::new();
        for &variant in all {
            let name = as_str(variant);
            assert!(seen.insert(name), "{name} repeats");
            assert_eq!(serde_json::to_value(variant).unwrap(), name);
            assert_eq!(
                serde_json::from_value::<T>(serde_json::Value::String(name.into())).unwrap(),
                variant
            );
            assert_eq!(parse(name), Some(variant));
        }
        assert_eq!(parse("no_such_name"), None);
        assert_eq!(parse(""), None);
        assert!(serde_json::from_value::<T>(serde_json::json!("no_such_name")).is_err());
    }

    #[test]
    fn every_wire_enum_spells_each_variant_once() {
        names_agree(&Format::ALL, Format::as_str, Format::parse);
        names_agree(&Side::ALL, Side::as_str, Side::parse);
        names_agree(&Status::ALL, Status::as_str, Status::parse);
        names_agree(&Rotation::ALL, Rotation::as_str, Rotation::parse);
        names_agree(&MatchState::ALL, MatchState::as_str, MatchState::parse);
        names_agree(
            &DeliveryState::ALL,
            DeliveryState::as_str,
            DeliveryState::parse,
        );
        names_agree(&ErrorCode::ALL, ErrorCode::as_str, ErrorCode::parse);
        names_agree(&Action::ALL, Action::as_str, Action::parse);
        names_agree(&Method::ALL, Method::as_str, Method::parse);
    }

    #[test]
    fn event_kinds_are_found_by_their_full_type() {
        let mut seen = std::collections::BTreeSet::new();
        for kind in Kind::ALL {
            assert!(seen.insert(kind.name()));
            assert_eq!(kind.name(), kind.as_str());
            assert_eq!(Kind::from_full(&kind.full()), Some(kind));
            assert_eq!(Kind::parse(kind.name()), Some(kind));
        }
        for text in [
            "",
            "io.ember.tournament.match.created",
            "io.ember.match.created.v1",
            "io.ember.tournament.match.created.v2",
            "io.ember.tournament.match.nope.v1",
            "com.ember.tournament.match.created.v1",
        ] {
            assert_eq!(Kind::from_full(text), None, "{text}");
        }
    }
}
